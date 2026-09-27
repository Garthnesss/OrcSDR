#include "frontend_lab.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include <esp_cpu.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "dsps_fir.h"
#include "frontend_coeffs.hpp"
#include "multirate.hpp"

namespace orcsdr::dsp::lab {
namespace {

using namespace orcsdr::dsp::mr;
namespace co = orcsdr::dsp::coeffs;

constexpr size_t kBlock = 16384;  // live IQ block, complex samples
constexpr uint32_t kRates[] = {2400000u, 2560000u, 2880000u, 3200000u};
constexpr double kOutRate = 240000.0;
constexpr double kCpuHz = 360e6;
constexpr int32_t kStageBound = 24170;  // documented worst |HB2 out| (Q15 stage units)

struct Cfg {
  Candidate c;
  PpKind p;
};
constexpr Cfg kCfgs[] = {
    {Candidate::a_float, PpKind::f32},       {Candidate::b_q15_full, PpKind::f32},
    {Candidate::b_q15_full, PpKind::q15},    {Candidate::c_espdsp_ansi, PpKind::f32},
    {Candidate::c_espdsp_ansi, PpKind::q15}, {Candidate::c_espdsp_arp4, PpKind::f32},
    {Candidate::c_espdsp_arp4, PpKind::q15}, {Candidate::d_q15_sparse, PpKind::f32},
    {Candidate::d_q15_sparse, PpKind::q15},  {Candidate::e_pie_pp, PpKind::q15},
    {Candidate::e_pie_all, PpKind::q15},
};
constexpr int kNumCfgs = sizeof(kCfgs) / sizeof(kCfgs[0]);

Emit g_emit = nullptr;
std::atomic<bool> g_busy{false};

void emitf(const char* fmt, ...) {
  char line[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  if (g_emit) g_emit(line);
}

void* palloc(size_t bytes) {
  return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

inline uint32_t cycles() { return esp_cpu_get_cycle_count(); }
inline void breathe() { vTaskDelay(1); }  // keep IDLE1 (task watchdog) fed

// ---- signal generation (DDS with a 4096-entry table, linear interp) ------

float g_sin[4097];
void init_table() {
  static bool ready = false;
  if (ready) return;
  for (int k = 0; k <= 4096; ++k) g_sin[k] = sinf(6.28318530718f * k / 4096.0f);
  ready = true;
}
inline float dds_sin(uint32_t ph) {
  const uint32_t idx = ph >> 20;
  const float frac = static_cast<float>(ph & 0xFFFFF) * (1.0f / 1048576.0f);
  return g_sin[idx] + (g_sin[idx + 1] - g_sin[idx]) * frac;
}
inline float dds_cos(uint32_t ph) { return dds_sin(ph + 0x40000000u); }

struct Rng {
  uint32_t s = 0x9E3779B9u;
  uint32_t next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  }
  float uni() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }
  float gauss() { return (uni() + uni() + uni() + uni() - 2.0f) * 1.7320508f; }
};

inline uint8_t q8(float v) {
  const long r = lroundf(128.0f + v);
  return static_cast<uint8_t>(r < 0 ? 0 : (r > 255 ? 255 : r));
}

struct Tone {
  double f;
  float amp;
};
struct Signal {
  const char* name;
  Tone tones[3];
  int ntones;
  float noise;      // gaussian sigma (CU8 LSB)
  bool impulse;
  bool dither;      // TPDF +-1 LSB
  int main_tone;    // index of the tone used for amplitude/phase, -1 none
  int interferer;   // index of an out-of-band tone to measure leakage, -1 none
};

// Exact frequency the DDS produces for f at fs.
inline uint32_t dds_inc(double f, uint32_t fs) {
  double r = f / fs;
  r -= std::floor(r);
  return static_cast<uint32_t>(llround(r * 4294967296.0) & 0xFFFFFFFFll);
}
inline double dds_freq(double f, uint32_t fs) {
  double fe = static_cast<double>(dds_inc(f, fs)) / 4294967296.0 * fs;
  if (fe >= fs / 2.0) fe -= fs;
  return fe;
}

void generate(const Signal& s, uint32_t fs, uint8_t* out, size_t n, uint32_t seed) {
  init_table();
  Rng rng;
  rng.s = seed | 1u;
  uint32_t ph[3] = {0x1234567u, 0x7654321u, 0x2468ACEu};
  uint32_t inc[3] = {};
  for (int t = 0; t < s.ntones; ++t) inc[t] = dds_inc(s.tones[t].f, fs);
  for (size_t k = 0; k < n; ++k) {
    float vi = 0.0f, vq = 0.0f;
    for (int t = 0; t < s.ntones; ++t) {
      vi += s.tones[t].amp * dds_cos(ph[t]);
      vq += s.tones[t].amp * dds_sin(ph[t]);
      ph[t] += inc[t];
    }
    if (s.noise > 0.0f) {
      vi += s.noise * rng.gauss();
      vq += s.noise * rng.gauss();
    }
    if (s.dither) {
      vi += rng.uni() - rng.uni();
      vq += rng.uni() - rng.uni();
    }
    if (s.impulse) vi = vq = (k == 1001) ? 127.0f : 0.0f;
    out[2 * k] = q8(vi);
    out[2 * k + 1] = q8(vq);
  }
}

// ---- measurement helpers ---------------------------------------------------

struct Coh {
  double amp, phase;
};
Coh coherent(const Cf32* y, size_t from, size_t to, double f) {
  double si = 0, sq = 0;  // float products, double sums
  for (size_t n = from; n < to; ++n) {
    double x = f * static_cast<double>(n) / kOutRate;
    x -= std::floor(x);
    const float c = cosf(6.28318530718f * static_cast<float>(x));
    const float s = sinf(6.28318530718f * static_cast<float>(x));
    si += y[n].i * c + y[n].q * s;   // y * e^{-j w n}
    sq += y[n].q * c - y[n].i * s;
  }
  const double cnt = static_cast<double>(to - from);
  return {std::sqrt(si * si + sq * sq) / cnt, std::atan2(sq, si)};
}

void fft(Cf32* x, int n) {
  for (int i = 1, j = 0; i < n; ++i) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(x[i], x[j]);
  }
  for (int len = 2; len <= n; len <<= 1) {
    const float ang = -6.28318530718f / len;
    for (int k = 0; k < len / 2; ++k) {
      const float wr = cosf(ang * k), wi = sinf(ang * k);
      for (int i = k; i < n; i += len) {
        Cf32& a = x[i];
        Cf32& b = x[i + len / 2];
        const float tr = b.i * wr - b.q * wi, ti = b.i * wi + b.q * wr;
        b.i = a.i - tr;
        b.q = a.q - ti;
        a.i += tr;
        a.q += ti;
      }
    }
  }
}

// Largest spectral line (amplitude, Hann) in y[from, from+N), optionally
// ignoring +-4 bins around f_ignore.
double peak_amp(const Cf32* y, size_t from, int N, Cf32* work, double f_ignore, bool ignore) {
  for (int k = 0; k < N; ++k) {
    const float w = 0.5f - 0.5f * cosf(6.28318530718f * k / N);
    work[k].i = y[from + k].i * w;
    work[k].q = y[from + k].q * w;
  }
  fft(work, N);
  int skip = -1000000;
  if (ignore) {
    double b = f_ignore / kOutRate * N;
    skip = static_cast<int>(lround(b));
    skip = ((skip % N) + N) % N;
  }
  double best = 0;
  for (int k = 0; k < N; ++k) {
    int d = std::abs(k - skip);
    d = std::min(d, N - d);
    if (ignore && d <= 4) continue;
    const double m = std::sqrt(static_cast<double>(work[k].i) * work[k].i +
                               static_cast<double>(work[k].q) * work[k].q);
    best = std::max(best, m);
  }
  return best / (N / 2.0);
}

inline double db(double v) { return 20.0 * std::log10(std::max(v, 1e-12)); }

uint32_t fnv(const void* data, size_t bytes, uint32_t h = 2166136261u) {
  const auto* p = static_cast<const uint8_t*>(data);
  for (size_t k = 0; k < bytes; ++k) h = (h ^ p[k]) * 16777619u;
  return h;
}

uint32_t percentile(uint32_t* v, int n, int pct) {
  if (n <= 0) return 0;
  std::sort(v, v + n);
  int idx = (n * pct + 99) / 100 - 1;
  return v[std::clamp(idx, 0, n - 1)];
}

void log_espdsp(const orcsdr::dsp::mr::Frontend& fe, const char* ctx);

// ---- BENCH -------------------------------------------------------------------

void bench_one(uint32_t rate, const Cfg& cfg, size_t chunk, bool internal, const uint8_t* blocks,
               int nblocks, int runs, Cf32* out, size_t cap, uint32_t* tot) {
  Frontend fe;
  if (!fe.init(rate, cfg.c, cfg.p, chunk, internal)) {
    emitf("LAB_BENCH_ERROR rate=%lu cand=%s pp=%s init_failed", (unsigned long)rate,
          candidate_name(cfg.c), pp_name(cfg.p));
    return;
  }
  if (rate == kRates[0] && chunk == 2048) log_espdsp(fe, "bench");
  StageCycles sum{};
  size_t outs = 0;
  for (int w = 0; w < 4; ++w) fe.process(blocks + (w % nblocks) * kBlock * 2, kBlock, out, cap);
  for (int r = 0; r < runs; ++r) {
    StageCycles c{};
    const uint32_t t0 = cycles();
    outs += fe.process(blocks + (r % nblocks) * kBlock * 2, kBlock, out, cap, &c);
    tot[r] = cycles() - t0;
    sum.hb1 += c.hb1;
    sum.hb2 += c.hb2;
    sum.pp += c.pp;
    sum.other += c.other;
    if ((r & 15) == 15) breathe();
  }
  uint64_t all = 0;
  uint32_t mx = 0;
  for (int r = 0; r < runs; ++r) {
    all += tot[r];
    mx = std::max(mx, tot[r]);
  }
  const double in_samples = static_cast<double>(kBlock) * runs;
  const double interval_us = kBlock * 1e6 / rate;
  const double avg_us = all / kCpuHz * 1e6 / runs;
  const uint32_t p95 = percentile(tot, runs, 95), p99 = percentile(tot, runs, 99);
  const MemUse m = fe.mem();
  emitf("LAB_BENCH rate=%lu cand=%s pp=%s chunk=%u mem=%s runs=%d interval_us=%.0f avg_us=%.1f "
        "p95_us=%.1f p99_us=%.1f max_us=%.1f load_pct=%.1f cyc_in=%.2f cyc_out=%.1f "
        "hb1_cyc_in=%.2f hb2_cyc_in=%.2f pp_cyc_out=%.1f other_cyc_in=%.2f outs_per_block=%.1f "
        "mem_int=%u mem_psram=%u arp4=%d",
        (unsigned long)rate, candidate_name(cfg.c), pp_name(cfg.p), (unsigned)chunk,
        internal ? "int" : "psram", runs, interval_us, avg_us, p95 / kCpuHz * 1e6,
        p99 / kCpuHz * 1e6, mx / kCpuHz * 1e6, 100.0 * avg_us / interval_us,
        all / in_samples, all / static_cast<double>(outs), sum.hb1 / in_samples,
        sum.hb2 / in_samples, sum.pp / static_cast<double>(outs), sum.other / in_samples,
        static_cast<double>(outs) / runs, (unsigned)m.internal, (unsigned)m.psram,
        fe.arp4_active() ? 1 : 0);
}

void job_bench(int runs) {
  constexpr int kNb = 4;
  auto* blocks = static_cast<uint8_t*>(palloc(kNb * kBlock * 2));
  const size_t cap = 2048;  // >= outputs of one 16K block at any plan
  auto* out = static_cast<Cf32*>(heap_caps_malloc(cap * sizeof(Cf32), MALLOC_CAP_INTERNAL));
  auto* tot = static_cast<uint32_t*>(palloc(sizeof(uint32_t) * runs));
  if (!blocks || !out || !tot) {
    emitf("LAB_BENCH_ERROR alloc");
  } else {
    Signal s{"bench", {{50e3, 40.0f}, {700e3, 30.0f}, {-230e3, 20.0f}}, 3, 20.0f, false, false, -1, -1};
    for (uint32_t rate : kRates) {
      generate(s, rate, blocks, kNb * kBlock, 7u);
      for (const Cfg& cfg : kCfgs) {
        bench_one(rate, cfg, 2048, true, blocks, kNb, runs, out, cap, tot);
        breathe();
      }
      // Buffer placement / chunking comparison on the main candidates.
      bench_one(rate, {Candidate::d_q15_sparse, PpKind::q15}, 16384, false, blocks, kNb, runs, out,
                cap, tot);
      bench_one(rate, {Candidate::d_q15_sparse, PpKind::f32}, 16384, false, blocks, kNb, runs, out,
                cap, tot);
      breathe();
    }
  }
  heap_caps_free(blocks);
  heap_caps_free(out);
  heap_caps_free(tot);
  emitf("LAB_BENCH_DONE");
}

// ---- TEST (numerics vs the float oracle) ----------------------------------

const Signal* test_signals(uint32_t fs, int* count) {
  static Signal s[9];
  const double fold = fs / 2.0 - 60e3;  // HB1 fold zone: lands at -60k after /2
  s[0] = {"impulse", {}, 0, 0.0f, true, false, -1, -1};
  s[1] = {"cw_37k", {{37.5e3, 90.0f}}, 1, 0.0f, false, false, 0, -1};
  s[2] = {"multitone", {{20e3, 35.0f}, {-55e3, 35.0f}, {90e3, 35.0f}}, 3, 0.0f, false, false, 0, -1};
  s[3] = {"band_edge_99k", {{99e3, 90.0f}}, 1, 0.0f, false, false, 0, -1};
  s[4] = {"adjacent_400k", {{30e3, 20.0f}, {400e3, 100.0f}}, 2, 0.0f, false, false, 0, 1};
  s[5] = {"hb1_fold", {{30e3, 20.0f}, {fold, 100.0f}}, 2, 0.0f, false, false, 0, 1};
  s[6] = {"fullscale_60k", {{60e3, 127.0f}}, 1, 0.0f, false, false, 0, -1};
  s[7] = {"noise", {}, 0, 40.0f, false, false, -1, -1};
  s[8] = {"clip_40k", {{40e3, 180.0f}}, 1, 0.0f, false, false, 0, -1};
  *count = 9;
  return s;
}

struct Buffers {
  uint8_t* in = nullptr;
  Cf32* ref = nullptr;
  Cf32* y = nullptr;
  Cf32* work = nullptr;
  size_t cap = 0;
  bool ok() const { return in && ref && y && work; }
};

Buffers alloc_buffers(size_t n_in) {
  Buffers b;
  b.cap = n_in / 8 + 256;  // >= n * 240k / 2.4M
  b.in = static_cast<uint8_t*>(palloc(n_in * 2));
  b.ref = static_cast<Cf32*>(palloc(b.cap * sizeof(Cf32)));
  b.y = static_cast<Cf32*>(palloc(b.cap * sizeof(Cf32)));
  b.work = static_cast<Cf32*>(palloc(4096 * sizeof(Cf32)));
  return b;
}
void free_buffers(Buffers& b) {
  heap_caps_free(b.in);
  heap_caps_free(b.ref);
  heap_caps_free(b.y);
  heap_caps_free(b.work);
  b = {};
}

size_t run(uint32_t rate, const Cfg& cfg, const uint8_t* in, size_t n, Cf32* out, size_t cap,
           size_t chunk = 2048, int32_t* peak = nullptr) {
  Frontend fe;
  if (!fe.init(rate, cfg.c, cfg.p, chunk, true)) return 0;
  fe.set_check(peak != nullptr);
  const size_t got = fe.process(in, n, out, cap);
  if (peak) *peak = fe.peak_q15();
  return got;
}

void job_test() {
  const size_t n = 4 * kBlock;
  Buffers b = alloc_buffers(n);
  if (!b.ok()) {
    emitf("LAB_TEST_ERROR alloc");
    free_buffers(b);
    emitf("LAB_TEST_DONE");
    return;
  }
  constexpr size_t kSkip = 64;
  for (uint32_t rate : kRates) {
    int ns = 0;
    const Signal* sigs = test_signals(rate, &ns);
    for (int si = 0; si < ns; ++si) {
      const Signal& s = sigs[si];
      generate(s, rate, b.in, n, 1000u + si);
      const size_t nr = run(rate, kCfgs[0], b.in, n, b.ref, b.cap);
      // Oracle's own accuracy for tones (vs the ideal input tone).
      double ref_amp = 0, ref_leak = 0;
      if (s.main_tone >= 0) {
        const double f = dds_freq(s.tones[s.main_tone].f, rate);
        ref_amp = coherent(b.ref, kSkip, nr, f).amp;
      }
      if (s.interferer >= 0) {
        const double f = dds_freq(s.tones[s.main_tone].f, rate);
        ref_leak = peak_amp(b.ref, kSkip, 4096, b.work, f, true);
      }
      for (int ci = 1; ci < kNumCfgs; ++ci) {
        int32_t peak = 0;
        const size_t ny =
            run(rate, kCfgs[ci], b.in, n, b.y, b.cap, 2048,
                kCfgs[ci].p == PpKind::q15 ? &peak : nullptr);
        // Best integer lag in [-2, 2].
        int best_lag = 0;
        double best_err = 1e300, sig = 0;
        if (nr < kSkip + 64 || ny < kSkip + 64) {
          emitf("LAB_TEST_ERROR rate=%lu sig=%s cand=%s no_output", (unsigned long)rate, s.name,
                candidate_name(kCfgs[ci].c));
          continue;
        }
        const size_t end = std::min(nr, ny) - 4;
        for (int lag = -2; lag <= 2; ++lag) {
          double e = 0, sp = 0;
          for (size_t k = kSkip; k < end; ++k) {
            const Cf32& r = b.ref[k];
            const Cf32& y = b.y[k + lag];
            const double di = y.i - r.i, dq = y.q - r.q;
            e += di * di + dq * dq;
            sp += static_cast<double>(r.i) * r.i + static_cast<double>(r.q) * r.q;
          }
          if (e < best_err) {
            best_err = e;
            best_lag = lag;
            sig = sp;
          }
        }
        float max_err = 0;
        for (size_t k = kSkip; k < end; ++k) {
          const Cf32& r = b.ref[k];
          const Cf32& y = b.y[k + best_lag];
          max_err = std::max(max_err, std::max(fabsf(y.i - r.i), fabsf(y.q - r.q)));
        }
        const double snr = best_err > 0 ? 10.0 * std::log10(sig / best_err) : 999.0;
        double amp_err = 0, ph_err = 0, leak = 0;
        if (s.main_tone >= 0) {
          const double f = dds_freq(s.tones[s.main_tone].f, rate);
          const Coh cr = coherent(b.ref, kSkip, end, f);
          const Coh cy = coherent(b.y + best_lag, kSkip, end, f);
          amp_err = db(cy.amp / cr.amp);
          ph_err = (cy.phase - cr.phase) * 57.2957795;
          while (ph_err > 180) ph_err -= 360;
          while (ph_err < -180) ph_err += 360;
        }
        if (s.interferer >= 0) {
          const double f = dds_freq(s.tones[s.main_tone].f, rate);
          leak = peak_amp(b.y, kSkip, 4096, b.work, f, true);
        }
        emitf("LAB_TEST rate=%lu sig=%s cand=%s pp=%s lag=%d snr_db=%.1f max_err=%.4f "
              "amp_err_db=%.4f phase_err_deg=%.3f ref_gain_db=%.4f leak_db=%.1f ref_leak_db=%.1f "
              "peak=%ld bound=%ld",
              (unsigned long)rate, s.name, candidate_name(kCfgs[ci].c), pp_name(kCfgs[ci].p),
              best_lag, snr, max_err, amp_err, ph_err,
              s.main_tone >= 0 ? db(ref_amp / s.tones[s.main_tone].amp) : 0.0,
              s.interferer >= 0 ? db(leak / s.tones[s.interferer].amp) : 0.0,
              s.interferer >= 0 ? db(ref_leak / s.tones[s.interferer].amp) : 0.0,
              (long)peak, (long)kStageBound);
        breathe();
      }
    }
  }
  free_buffers(b);
  emitf("LAB_TEST_DONE");
}

// ---- SWEEP (measured response incl. aliasing) --------------------------------

void job_sweep(int rate_sel, int cfg_sel) {
  const size_t n = 4 * kBlock;
  Buffers b = alloc_buffers(n);
  if (!b.ok()) {
    emitf("LAB_SWEEP_ERROR alloc");
    free_buffers(b);
    emitf("LAB_SWEEP_DONE");
    return;
  }
  constexpr size_t kSkip = 128;
  constexpr float kAmp = 100.0f;
  for (int ri = 0; ri < 4; ++ri) {
    if (rate_sel >= 0 && rate_sel != ri) continue;
    const uint32_t rate = kRates[ri];
    // Dense +-250 kHz, coarse over the rest of the band.
    double freqs[256];
    int nf = 0;
    for (double f = -250e3; f <= 250e3 + 1; f += 5e3) freqs[nf++] = f;
    for (int k = -32; k < 32; ++k) {
      const double f = k * (rate / 64.0) + rate / 128.0;
      if (std::fabs(f) > 250e3) freqs[nf++] = f;
    }
    for (int fi = 0; fi < nf; ++fi) {
      Signal s{"sweep", {{freqs[fi], kAmp}}, 1, 0.0f, false, true, 0, -1};
      generate(s, rate, b.in, n, 77u + fi);
      const double fe = dds_freq(freqs[fi], rate);
      for (int ci = 0; ci < kNumCfgs; ++ci) {
        if (cfg_sel >= 0 && cfg_sel != ci) continue;
        const size_t ny = run(rate, kCfgs[ci], b.in, n, b.y, b.cap);
        const bool inband = std::fabs(fe) < 118e3;
        const double gain = inband ? coherent(b.y, kSkip, ny, fe).amp / kAmp : 0.0;
        const double peak = peak_amp(b.y, kSkip, 4096, b.work, 0, false) / kAmp;
        emitf("LAB_SWEEP rate=%lu cand=%s pp=%s f=%.0f gain_db=%.4f peak_db=%.2f",
              (unsigned long)rate, candidate_name(kCfgs[ci].c), pp_name(kCfgs[ci].p), fe,
              inband ? db(gain) : -999.0, db(peak));
      }
      breathe();
    }
  }
  free_buffers(b);
  emitf("LAB_SWEEP_DONE");
}

// ---- CONT (block-boundary continuity) ------------------------------------------

void job_cont() {
  const size_t n = 4 * kBlock;
  Buffers b = alloc_buffers(n);
  if (!b.ok()) {
    emitf("LAB_CONT_ERROR alloc");
    free_buffers(b);
    emitf("LAB_CONT_DONE");
    return;
  }
  static constexpr size_t kPieces[] = {4, 1000, 2048, 12, 16384, 36, 7000, 4, 3072, 20};
  for (uint32_t rate : kRates) {
    Signal s{"cont", {{41e3, 60.0f}, {-333e3, 40.0f}}, 2, 15.0f, false, false, -1, -1};
    generate(s, rate, b.in, n, 4242u);
    for (int ci = 0; ci < kNumCfgs; ++ci) {
      const size_t n_one = run(rate, kCfgs[ci], b.in, n, b.ref, b.cap, 2048);
      // Same stream, fed in irregular pieces through a 16384-sample chunking.
      Frontend fe;
      size_t n_pieces = 0;
      if (fe.init(rate, kCfgs[ci].c, kCfgs[ci].p, 16384, false)) {
        size_t pos = 0;
        int k = 0;
        while (pos < n) {
          const size_t take = std::min(kPieces[k++ % 10], n - pos);
          n_pieces += fe.process(b.in + pos * 2, take, b.y + n_pieces, b.cap - n_pieces);
          pos += take;
        }
      }
      const size_t cmp = std::min(n_one, n_pieces);
      const bool same = n_one == n_pieces && memcmp(b.ref, b.y, cmp * sizeof(Cf32)) == 0;
      size_t first_diff = cmp;
      for (size_t k = 0; k < cmp; ++k)
        if (memcmp(&b.ref[k], &b.y[k], sizeof(Cf32)) != 0) {
          first_diff = k;
          break;
        }
      emitf("LAB_CONT rate=%lu cand=%s pp=%s outputs=%u/%u bit_identical=%d first_diff=%u",
            (unsigned long)rate, candidate_name(kCfgs[ci].c), pp_name(kCfgs[ci].p),
            (unsigned)n_one, (unsigned)n_pieces, same ? 1 : 0, (unsigned)first_diff);
      breathe();
    }
    for (const Candidate ec : {Candidate::e_pie_pp, Candidate::e_pie_all}) {
      // E must equal D + pp_q15 exactly (same integer sums, PIE vs scalar).
      const size_t nd = run(rate, {Candidate::d_q15_sparse, PpKind::q15}, b.in, n, b.ref, b.cap);
      const size_t ne = run(rate, {ec, PpKind::q15}, b.in, n, b.y, b.cap);
      size_t first = std::min(nd, ne);
      for (size_t k = 0; k < std::min(nd, ne); ++k)
        if (memcmp(&b.ref[k], &b.y[k], sizeof(Cf32)) != 0) { first = k; break; }
      const bool same = nd == ne && nd > 0 && first == nd;
      emitf("LAB_CONT_E_VS_D rate=%lu cand=%s outputs=%u/%u bit_identical=%d first_diff=%u",
            (unsigned long)rate, candidate_name(ec), (unsigned)nd, (unsigned)ne, same ? 1 : 0,
            (unsigned)first);
    }
  }
  free_buffers(b);
  emitf("LAB_CONT_DONE");
}

// ---- PREEMPT (forced preemption, HWLOOP + FPU state) --------------------------

struct Aggressor {
  TaskHandle_t task = nullptr;
  esp_timer_handle_t timer = nullptr;
  std::atomic<bool> stop{false};
  std::atomic<uint32_t> runs{0}, bad{0};
  fir_s16_t fir{};
  int16_t* coeffs = nullptr;
  int16_t* in = nullptr;
  int16_t* out = nullptr;
  uint32_t golden = 0;
  float fgolden = 0;
};
Aggressor* g_agg = nullptr;

void agg_timer_cb(void*) {
  if (g_agg && g_agg->task) xTaskNotifyGive(g_agg->task);
}

float agg_float_work(const int16_t* in) {
  float a = 0.0f, b = 0.0f;
  for (int k = 0; k < 256; k += 2) {
    a += 0.001f * in[k];
    b += 0.002f * in[k + 1];
  }
  return a - b;
}

void agg_task(void* arg) {
  auto* a = static_cast<Aggressor*>(arg);
  while (!a->stop.load()) {
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50)) == 0) continue;
    memset(a->fir.delay, 0, sizeof(int16_t) * a->fir.coeffs_len);
    a->fir.pos = 0;
    a->fir.d_pos = 0;
    dsps_fird_s16_arp4(&a->fir, a->in, a->out, 128);  // HWLOOP kernel
    const uint32_t h = fnv(a->out, 128 * sizeof(int16_t));
    const float f = agg_float_work(a->in);
    a->runs.fetch_add(1);
    if (h != a->golden || f != a->fgolden) a->bad.fetch_add(1);
  }
  a->task = nullptr;
  vTaskDelete(nullptr);
}

// Starts the aggressor: a priority-8 task on core 1, woken by an esp_timer
// every period_us, running an arp4 (PIE + HWLOOP) FIR and float work and
// checking that its repeated runs stay identical to its first run.
Aggressor* agg_begin() {
  auto* a = new Aggressor();
  a->coeffs = static_cast<int16_t*>(heap_caps_aligned_alloc(16, 16 * sizeof(int16_t), MALLOC_CAP_INTERNAL));
  a->in = static_cast<int16_t*>(heap_caps_aligned_alloc(16, 256 * sizeof(int16_t), MALLOC_CAP_INTERNAL));
  a->out = static_cast<int16_t*>(heap_caps_aligned_alloc(16, 128 * sizeof(int16_t), MALLOC_CAP_INTERNAL));
  if (!a->coeffs || !a->in || !a->out) {
    heap_caps_free(a->coeffs);
    heap_caps_free(a->in);
    heap_caps_free(a->out);
    delete a;
    return nullptr;
  }
  for (int k = 0; k < 16; ++k) a->coeffs[k] = k == 0 ? 0 : co::kHb2Q15[k - 1];
  Rng rng;
  for (int k = 0; k < 256; ++k) a->in[k] = static_cast<int16_t>(rng.next() & 0x3FFF) - 8192;
  dsps_fird_init_s16(&a->fir, a->coeffs, nullptr, 16, 2, 0, 0);
  {
    int16_t rev[16] __attribute__((aligned(16)));
    for (int k = 0; k < 16; ++k) rev[k] = a->coeffs[15 - k];
    fir_s16_t ref{};
    dsps_fird_init_s16(&ref, rev, nullptr, 16, 2, 0, 0);
    dsps_fird_s16_ansi(&ref, a->in, a->out, 128);
    dsps_fird_s16_aexx_free(&ref);
  }
  a->golden = fnv(a->out, 128 * sizeof(int16_t));
  a->fgolden = agg_float_work(a->in);
  g_agg = a;
  xTaskCreatePinnedToCore(agg_task, "lab_agg", 4096, a, 8, &a->task, 1);
  esp_timer_create_args_t targs{};
  targs.callback = agg_timer_cb;
  targs.name = "lab_agg";
  esp_timer_create(&targs, &a->timer);
  return a;
}

void agg_end(Aggressor* a) {
  if (!a) return;
  esp_timer_stop(a->timer);
  a->stop.store(true);
  for (int k = 0; k < 20 && a->task != nullptr; ++k) vTaskDelay(pdMS_TO_TICKS(20));
  esp_timer_delete(a->timer);
  g_agg = nullptr;
  dsps_fird_s16_aexx_free(&a->fir);
  heap_caps_free(a->coeffs);
  heap_caps_free(a->in);
  heap_caps_free(a->out);
  delete a;
}

void job_preempt(int seconds, int period_us) {
  const size_t n = kBlock;  // one block per victim iteration
  Buffers b = alloc_buffers(n);
  Aggressor* a = b.ok() ? agg_begin() : nullptr;
  if (!a) {
    emitf("LAB_PREEMPT_ERROR alloc");
  } else {
    Signal s{"preempt", {{41e3, 60.0f}, {-333e3, 40.0f}}, 2, 15.0f, false, false, -1, -1};
    const Cfg victims[] = {{Candidate::c_espdsp_arp4, PpKind::q15},
                           {Candidate::c_espdsp_ansi, PpKind::q15},
                           {Candidate::d_q15_sparse, PpKind::q15},
                           {Candidate::d_q15_sparse, PpKind::f32},
                           {Candidate::e_pie_pp, PpKind::q15},
                           {Candidate::e_pie_all, PpKind::q15},
                           {Candidate::a_float, PpKind::f32}};
    const uint32_t rate = 3200000u;
    generate(s, rate, b.in, n, 99u);
    for (const Cfg& v : victims) {
      Frontend fe;
      if (!fe.init(rate, v.c, v.p, 2048, true)) continue;
      const size_t ng = fe.process(b.in, n, b.ref, b.cap);
      const uint32_t golden = fnv(b.ref, ng * sizeof(Cf32));
      const uint32_t runs0 = a->runs.load(), bad0 = a->bad.load();
      esp_timer_start_periodic(a->timer, period_us);
      const int64_t t_end = esp_timer_get_time() + static_cast<int64_t>(seconds) * 1000000;
      uint32_t iters = 0, mism = 0;
      while (esp_timer_get_time() < t_end) {
        fe.reset();
        const size_t ny = fe.process(b.in, n, b.y, b.cap);
        if (ny != ng || fnv(b.y, ny * sizeof(Cf32)) != golden) ++mism;
        ++iters;
        breathe();
      }
      esp_timer_stop(a->timer);
      emitf("LAB_PREEMPT cand=%s pp=%s arp4=%d seconds=%d period_us=%d victim_iters=%lu "
            "victim_mismatch=%lu aggressor_runs=%lu aggressor_mismatch=%lu",
            candidate_name(v.c), pp_name(v.p), fe.arp4_active() ? 1 : 0, seconds, period_us,
            (unsigned long)iters, (unsigned long)mism,
            (unsigned long)(a->runs.load() - runs0), (unsigned long)(a->bad.load() - bad0));
    }
  }
  agg_end(a);
  free_buffers(b);
  emitf("LAB_PREEMPT_DONE");
}

// ---- SOAK (long comparison against the oracle) ---------------------------------
//
// Each iteration: a rate (rotating), a random chunk size (2048 internal /
// 16384 PSRAM) and random piece boundaries. The output must be bit-identical
// to the golden: for arp4 the ESP-DSP ANSI chain (same arithmetic), for every
// other candidate its own single-pass output (determinism + continuity).
// Optional aggressor forces preemption every period_us (0 = none).
void job_soak(int minutes, int cfg_index, int period_us) {
  const Cfg cfg = kCfgs[std::clamp(cfg_index, 0, kNumCfgs - 1)];
  const size_t n = 2 * kBlock;
  Buffers b = alloc_buffers(n);
  auto* golden = static_cast<Cf32*>(palloc(4 * b.cap * sizeof(Cf32)));
  auto* inputs = static_cast<uint8_t*>(palloc(4 * n * 2));
  size_t gn[4] = {};
  bool ok = b.ok() && golden && inputs;
  for (int r = 0; ok && r < 4; ++r) {
    Signal s{"soak", {{41e3, 60.0f}, {-333e3, 40.0f}, {97e3, 30.0f}}, 3, 20.0f, false, false, -1, -1};
    generate(s, kRates[r], inputs + r * n * 2, n, 555u + r);
    const Cfg gcfg =
        cfg.c == Candidate::c_espdsp_arp4 ? Cfg{Candidate::c_espdsp_ansi, cfg.p} : cfg;
    gn[r] = run(kRates[r], gcfg, inputs + r * n * 2, n, golden + r * b.cap, b.cap, 2048);
    ok = gn[r] > 0;
  }
  if (!ok) emitf("LAB_SOAK_ERROR setup");
  Aggressor* a = (ok && period_us > 0) ? agg_begin() : nullptr;
  if (a) esp_timer_start_periodic(a->timer, period_us);
  Rng rng;
  rng.s = 0xC0FFEEu;
  uint32_t iters = 0, mism = 0, outputs_bad = 0;
  const int64_t t0 = esp_timer_get_time();
  const int64_t t_end = t0 + static_cast<int64_t>(minutes) * 60000000;
  int64_t next_report = t0 + 60000000;
  while (ok && esp_timer_get_time() < t_end) {
    const int r = static_cast<int>(iters % 4);
    const bool big = (rng.next() & 1) != 0;
    Frontend fe;
    if (!fe.init(kRates[r], cfg.c, cfg.p, big ? 16384 : 2048, !big)) {
      emitf("LAB_SOAK_ERROR init_failed rate=%lu", (unsigned long)kRates[r]);
      break;
    }
    const uint8_t* in = inputs + r * n * 2;
    size_t pos = 0, got = 0;
    while (pos < n) {
      const size_t take = std::min<size_t>(4 * (1 + rng.next() % 5000), n - pos);
      got += fe.process(in + pos * 2, take, b.y + got, b.cap - got);
      pos += take;
    }
    const Cf32* g = golden + r * b.cap;
    size_t first = 0;
    uint32_t bad = got == gn[r] ? 0 : 1;
    for (size_t k = 0; k < std::min(got, gn[r]); ++k)
      if (memcmp(&g[k], &b.y[k], sizeof(Cf32)) != 0) {
        if (bad == 0) first = k;
        ++bad;
      }
    if (bad) {
      ++mism;
      outputs_bad += bad;
      if (mism <= 5)
        emitf("LAB_SOAK_MISMATCH iter=%lu rate=%lu chunk=%s outputs=%u/%u bad_outputs=%lu "
              "first=%u golden=%.4f,%.4f got=%.4f,%.4f",
              (unsigned long)iters, (unsigned long)kRates[r], big ? "16384/psram" : "2048/int",
              (unsigned)got, (unsigned)gn[r], (unsigned long)bad, (unsigned)first,
              first < gn[r] ? g[first].i : 0.0f, first < gn[r] ? g[first].q : 0.0f,
              first < got ? b.y[first].i : 0.0f, first < got ? b.y[first].q : 0.0f);
    }
    ++iters;
    breathe();
    if (esp_timer_get_time() >= next_report) {
      next_report += 60000000;
      emitf("LAB_SOAK_PROGRESS cand=%s pp=%s minutes=%.1f iters=%lu mismatched_iters=%lu "
            "aggressor_runs=%lu aggressor_mismatch=%lu",
            candidate_name(cfg.c), pp_name(cfg.p), (esp_timer_get_time() - t0) / 60e6,
            (unsigned long)iters, (unsigned long)mism, a ? (unsigned long)a->runs.load() : 0ul,
            a ? (unsigned long)a->bad.load() : 0ul);
    }
  }
  emitf("LAB_SOAK cand=%s pp=%s minutes=%d period_us=%d iters=%lu mismatched_iters=%lu "
        "bad_outputs=%lu aggressor_runs=%lu aggressor_mismatch=%lu",
        candidate_name(cfg.c), pp_name(cfg.p), minutes, period_us, (unsigned long)iters,
        (unsigned long)mism, (unsigned long)outputs_bad,
        a ? (unsigned long)a->runs.load() : 0ul, a ? (unsigned long)a->bad.load() : 0ul);
  agg_end(a);
  heap_caps_free(inputs);
  heap_caps_free(golden);
  free_buffers(b);
  emitf("LAB_SOAK_DONE");
}

void log_espdsp(const Frontend& fe, const char* ctx) {
  static const char* kNames[] = {"hb1_i", "hb1_q", "hb2_i", "hb2_q"};
  const auto mem = [](const void* p) {
    return esp_ptr_internal(p) ? "int" : (esp_ptr_external_ram(p) ? "psram" : "?");
  };
  for (int k = 0; k < 4; ++k) {
    const auto* f = static_cast<const fir_s16_t*>(fe.espdsp_fir(k));
    if (!f) return;
    emitf("LAB_ESPDSP_INIT ctx=%s filter=%s arp4=%d delay=%p delay_align16=%u delay_mem=%s "
          "coeffs=%p coeffs_align16=%u coeffs_mem=%s coeffs_len=%d decim=%d shift=%d pos=%d "
          "d_pos=%d start_pos=%d rounding_val=%ld rounding_buff=%p free_status=0x%x",
          ctx, kNames[k], fe.arp4_active() ? 1 : 0, f->delay,
          static_cast<unsigned>(reinterpret_cast<uintptr_t>(f->delay) & 15), mem(f->delay),
          f->coeffs, static_cast<unsigned>(reinterpret_cast<uintptr_t>(f->coeffs) & 15),
          mem(f->coeffs), f->coeffs_len, f->decim, f->shift, f->pos, f->d_pos, f->start_pos,
          static_cast<long>(f->rounding_val), f->rounding_buff, f->free_status);
  }
}

// ---- ARP4 (unit test of dsps_fird_s16_arp4 against dsps_fird_s16_ansi) -------

struct FirRig {
  fir_s16_t fir{};
  bool init(const int16_t* c, int len, int decim, int shift) {
    return dsps_fird_init_s16(&fir, const_cast<int16_t*>(c), nullptr, len, decim, 0, shift) == ESP_OK;
  }
  ~FirRig() { dsps_fird_s16_aexx_free(&fir); }
};

void job_arp4() {
  constexpr int kMaxLen = 16, kIn = 512, kOut = kIn / 2;
  for (const int shape : {0, 1}) {
  const int kLen = shape == 0 ? 16 : 8;
  const int kShift = shape == 0 ? 0 : 7;
  auto* c = static_cast<int16_t*>(heap_caps_aligned_alloc(16, kMaxLen * 2, MALLOC_CAP_INTERNAL));
  auto* cr = static_cast<int16_t*>(heap_caps_aligned_alloc(16, kMaxLen * 2, MALLOC_CAP_INTERNAL));
  auto* in = static_cast<int16_t*>(heap_caps_aligned_alloc(16, kIn * 2, MALLOC_CAP_INTERNAL));
  auto* ya = static_cast<int16_t*>(heap_caps_aligned_alloc(16, kOut * 2, MALLOC_CAP_INTERNAL));
  auto* yb = static_cast<int16_t*>(heap_caps_aligned_alloc(16, kOut * 2, MALLOC_CAP_INTERNAL));
  if (!c || !cr || !in || !ya || !yb) {
    emitf("LAB_ARP4_ERROR alloc");
  } else {
    // Asymmetric taps so ordering mistakes show up.
    for (int k = 0; k < kLen; ++k) {
      c[k] = static_cast<int16_t>((k + 1) * 700 - (k % 3) * 1500);
      cr[kLen - 1 - k] = c[k];
    }
    Rng rng;
    for (int k = 0; k < kIn; ++k)
      in[k] = static_cast<int16_t>(shape == 0 ? (rng.next() & 0x3FFF) - 8192
                                              : static_cast<int32_t>(rng.next() & 0xFF) - 128);
    struct Case {
      const char* name;
      bool reversed;
      int split;  // outputs per call (0 = one call); -1 = pattern 1,2,3,1,2,3...
      bool psram;
    } cases[] = {{"same_order", false, 0, false},         {"reversed", true, 0, false},
                 {"reversed_split_7", true, 7, false},     {"reversed_split_64", true, 64, false},
                 {"reversed_calls_1", true, 1, false},     {"reversed_calls_1_2_3", true, -1, false},
                 {"reversed_psram_io", true, 0, true},     {"reversed_psram_calls_1_2_3", true, -1, true}};
    for (const Case& cs : cases) {
      FirRig ra, rb;
      ra.init(c, kLen, 2, kShift);
      rb.init(cs.reversed ? cr : c, kLen, 2, kShift);
      dsps_fird_s16_ansi(&ra.fir, in, ya, kOut);
      int16_t* src = in;
      int16_t* dst = yb;
      int16_t* pin = nullptr;
      int16_t* pout = nullptr;
      if (cs.psram) {
        pin = static_cast<int16_t*>(heap_caps_aligned_alloc(16, kIn * 2, MALLOC_CAP_SPIRAM));
        pout = static_cast<int16_t*>(heap_caps_aligned_alloc(16, kOut * 2, MALLOC_CAP_SPIRAM));
        if (!pin || !pout) continue;
        memcpy(pin, in, kIn * 2);
        src = pin;
        dst = pout;
      }
      if (cs.split == 0) {
        dsps_fird_s16_arp4(&rb.fir, src, dst, kOut);
      } else if (cs.split == 7 || cs.split == 64) {
        dsps_fird_s16_arp4(&rb.fir, src, dst, cs.split);
        dsps_fird_s16_arp4(&rb.fir, src + cs.split * 2, dst + cs.split, kOut - cs.split);
      } else {
        int done = 0, step = 0;
        while (done < kOut) {
          const int take = std::min(cs.split > 0 ? cs.split : 1 + (step++ % 3), kOut - done);
          dsps_fird_s16_arp4(&rb.fir, src + done * 2, dst + done, take);
          done += take;
        }
      }
      if (cs.psram) memcpy(yb, pout, kOut * 2);
      heap_caps_free(pin);
      heap_caps_free(pout);
      int diff = 0, first = -1, maxd = 0, lag_hits[5] = {};
      for (int k = 0; k < kOut; ++k) {
        const int d = std::abs(ya[k] - yb[k]);
        if (d) {
          ++diff;
          if (first < 0) first = k;
          maxd = std::max(maxd, d);
        }
        for (int l = -2; l <= 2; ++l)
          if (k + l >= 0 && k + l < kOut && ya[k] == yb[k + l]) ++lag_hits[l + 2];
      }
      emitf("LAB_ARP4 taps=%d shift=%d case=%s outputs=%d diff=%d first_diff=%d max_abs_diff=%d "
            "match_lag[-2..2]=%d,%d,%d,%d,%d ansi[0..3]=%d,%d,%d,%d arp4[0..3]=%d,%d,%d,%d",
            kLen, kShift, cs.name, kOut, diff, first, maxd, lag_hits[0], lag_hits[1], lag_hits[2],
            lag_hits[3], lag_hits[4], ya[0], ya[1], ya[2], ya[3], yb[0], yb[1], yb[2], yb[3]);
    }
  }
  heap_caps_free(c);
  heap_caps_free(cr);
  heap_caps_free(in);
  heap_caps_free(ya);
  heap_caps_free(yb);
  }
  // Whole chain: ESP-DSP ANSI vs arp4 with identical chunking must match.
  {
    const size_t n = 4 * kBlock;
    Buffers b = alloc_buffers(n);
    if (b.ok()) {
      Signal s{"arp4", {{41e3, 60.0f}, {-333e3, 40.0f}}, 2, 15.0f, false, false, -1, -1};
      generate(s, 2400000u, b.in, n, 4242u);
      for (const bool internal : {true, false}) {
        for (const size_t chunk : {size_t{2048}, size_t{16384}}) {
          Frontend fa, fb;
          if (!fa.init(2400000u, Candidate::c_espdsp_ansi, PpKind::q15, chunk, internal) ||
              !fb.init(2400000u, Candidate::c_espdsp_arp4, PpKind::q15, chunk, internal)) {
            emitf("LAB_ARP4_CHAIN chunk=%u mem=%s init_failed", (unsigned)chunk,
                  internal ? "int" : "psram");
            continue;
          }
          log_espdsp(fa, "arp4_chain_ansi");
          log_espdsp(fb, "arp4_chain_arp4");
          const size_t na = fa.process(b.in, n, b.ref, b.cap);
          const size_t nb = fb.process(b.in, n, b.y, b.cap);
          size_t first = std::min(na, nb);
          for (size_t k = 0; k < std::min(na, nb); ++k)
            if (memcmp(&b.ref[k], &b.y[k], sizeof(Cf32)) != 0) { first = k; break; }
          emitf("LAB_ARP4_CHAIN chunk=%u mem=%s outputs=%u/%u first_diff=%u",
                (unsigned)chunk, internal ? "int" : "psram", (unsigned)na, (unsigned)nb,
                (unsigned)first);
        }
      }
    }
    free_buffers(b);
  }
  emitf("LAB_ARP4_DONE");
}

// ---- job runner ----------------------------------------------------------------

struct Job {
  char kind[12];
  int a, b;
};
Job g_job{};
int g_job_c = 0;

void lab_task(void*) {
  const Job j = g_job;
  if (!strcmp(j.kind, "BENCH")) job_bench(j.a > 0 ? j.a : 256);
  else if (!strcmp(j.kind, "TEST")) job_test();
  else if (!strcmp(j.kind, "SWEEP")) job_sweep(j.a, j.b);
  else if (!strcmp(j.kind, "CONT")) job_cont();
  else if (!strcmp(j.kind, "ARP4")) job_arp4();
  else if (!strcmp(j.kind, "SOAK")) job_soak(j.a > 0 ? j.a : 5, j.b, g_job_c);
  else if (!strcmp(j.kind, "PREEMPT")) job_preempt(j.a > 0 ? j.a : 10, j.b > 0 ? j.b : 250);
  g_busy.store(false);
  vTaskDelete(nullptr);
}

// ---- SHADOW (live timing in the DSP task) -------------------------------------

constexpr uint32_t kRing = 8192;
std::atomic<int> g_shadow_want{-1};
int g_shadow_cfg = -1;
uint32_t g_shadow_rate = 0;
Frontend* g_shadow_fe = nullptr;
Cf32* g_shadow_out = nullptr;
uint16_t* g_ring_fe = nullptr;
uint16_t* g_ring_total = nullptr;
std::atomic<uint32_t> g_ring_n{0};
bool g_shadow_pending = false;
uint32_t g_shadow_cyc = 0;

void shadow_stats() {
  const uint32_t n = std::min(g_ring_n.load(), kRing);
  if (n == 0 || !g_ring_fe) {
    emitf("LAB_SHADOW_STATS blocks=0");
    return;
  }
  auto* a = static_cast<uint32_t*>(palloc(sizeof(uint32_t) * n));
  auto* t = static_cast<uint32_t*>(palloc(sizeof(uint32_t) * n));
  if (!a || !t) {
    heap_caps_free(a);
    heap_caps_free(t);
    emitf("LAB_SHADOW_STATS error=alloc");
    return;
  }
  uint64_t sa = 0, st = 0;
  uint32_t over = 0;
  const double interval = g_shadow_rate ? kBlock * 1e6 / g_shadow_rate : 0;
  for (uint32_t k = 0; k < n; ++k) {
    a[k] = g_ring_fe[k];
    t[k] = g_ring_total[k];
    sa += a[k];
    st += t[k];
    if (interval > 0 && t[k] > interval) ++over;
  }
  const uint32_t amax = *std::max_element(a, a + n), tmax = *std::max_element(t, t + n);
  emitf("LAB_SHADOW_STATS cfg=%s/%s rate=%lu blocks=%lu interval_us=%.0f fe_avg_us=%.1f "
        "fe_p95=%lu fe_p99=%lu fe_max=%lu total_avg_us=%.1f total_p95=%lu total_p99=%lu "
        "total_max=%lu blocks_over_interval=%lu",
        g_shadow_cfg >= 0 ? candidate_name(kCfgs[g_shadow_cfg].c) : "off",
        g_shadow_cfg >= 0 ? pp_name(kCfgs[g_shadow_cfg].p) : "-", (unsigned long)g_shadow_rate,
        (unsigned long)n, interval, static_cast<double>(sa) / n, (unsigned long)percentile(a, n, 95),
        (unsigned long)percentile(a, n, 99), (unsigned long)amax, static_cast<double>(st) / n,
        (unsigned long)percentile(t, n, 95), (unsigned long)percentile(t, n, 99),
        (unsigned long)tmax, (unsigned long)over);
  heap_caps_free(a);
  heap_caps_free(t);
}

int parse_cfg(const char* cand, const char* pp) {
  for (int k = 0; k < kNumCfgs; ++k) {
    const char* name = candidate_name(kCfgs[k].c);
    if ((strcasecmp(cand, name) == 0 || (strlen(cand) == 1 && toupper(cand[0]) == name[0] &&
                                          kCfgs[k].c != Candidate::c_espdsp_ansi)) &&
        strcasecmp(pp, pp_name(kCfgs[k].p) + 3) == 0)
      return k;
  }
  return -1;
}

}  // namespace

void command(const char* args, bool radio_running, Emit emit) {
  g_emit = emit;
  char kind[12] = {}, x[24] = {}, y[24] = {}, z[24] = {};
  const int fields = sscanf(args, "%11s %23s %23s %23s", kind, x, y, z);
  if (fields < 1) {
    emitf("LAB_USAGE BENCH [runs] | TEST | SWEEP <0-3|-1> <cfg|-1> | CONT | PREEMPT [s] [period_us] | "
          "SHADOW <cand> <f32|q15> | SHADOW OFF | SHADOW STATS | SHADOW RESET | CFGS | ARP4 | "
          "SOAK <minutes> <cfg> [preempt_period_us]");
    return;
  }
  if (!strcmp(kind, "CFGS")) {
    for (int k = 0; k < kNumCfgs; ++k)
      emitf("LAB_CFG %d %s %s", k, candidate_name(kCfgs[k].c), pp_name(kCfgs[k].p));
    return;
  }
  if (!strcmp(kind, "SHADOW")) {
    if (!strcmp(x, "OFF")) {
      g_shadow_want.store(-1);
      emitf("LAB_SHADOW off");
    } else if (!strcmp(x, "STATS")) {
      shadow_stats();
    } else if (!strcmp(x, "RESET")) {
      g_ring_n.store(0);
      emitf("LAB_SHADOW reset");
    } else {
      const int cfg = parse_cfg(x, y);
      if (cfg < 0) {
        emitf("LAB_SHADOW_ERROR unknown_cfg (use e.g. D q15, C_espdsp_arp4 q15, A f32)");
        return;
      }
      g_ring_n.store(0);
      g_shadow_want.store(cfg);
      emitf("LAB_SHADOW on cfg=%s/%s (applied at the next block)", candidate_name(kCfgs[cfg].c),
            pp_name(kCfgs[cfg].p));
    }
    return;
  }
  if (radio_running) {
    emitf("LAB_ERROR radio_running (stop the radio first; SHADOW works while streaming)");
    return;
  }
  if (g_busy.exchange(true)) {
    emitf("LAB_ERROR busy");
    return;
  }
  strlcpy(g_job.kind, kind, sizeof(g_job.kind));
  g_job.a = fields >= 2 ? atoi(x) : 0;
  g_job.b = fields >= 3 ? atoi(y) : -1;
  g_job_c = fields >= 4 ? atoi(z) : 0;
  if (!strcmp(kind, "SWEEP")) {
    if (fields < 2) g_job.a = -1;
  }
  if (xTaskCreatePinnedToCore(lab_task, "dsp_lab", 8192, nullptr, 5, nullptr, 1) != pdPASS) {
    g_busy.store(false);
    emitf("LAB_ERROR task_create");
    return;
  }
  emitf("LAB_STARTED %s", kind);
}

void shadow_block(const uint8_t* cu8, size_t bytes, uint32_t sample_rate) {
  const int want = g_shadow_want.load(std::memory_order_relaxed);
  g_shadow_pending = false;
  if (want < 0) {
    if (g_shadow_fe) {
      delete g_shadow_fe;
      g_shadow_fe = nullptr;
      g_shadow_cfg = -1;
    }
    return;
  }
  if (!g_ring_fe) {
    g_ring_fe = static_cast<uint16_t*>(palloc(sizeof(uint16_t) * kRing));
    g_ring_total = static_cast<uint16_t*>(palloc(sizeof(uint16_t) * kRing));
    g_shadow_out = static_cast<Cf32*>(palloc(sizeof(Cf32) * 2048));
    if (!g_ring_fe || !g_ring_total || !g_shadow_out) {
      g_shadow_want.store(-1);
      return;
    }
  }
  if (!g_shadow_fe) g_shadow_fe = new Frontend();
  if (want != g_shadow_cfg || sample_rate != g_shadow_rate) {
    g_shadow_cfg = want;
    g_shadow_rate = sample_rate;
    if (!g_shadow_fe->init(sample_rate, kCfgs[want].c, kCfgs[want].p, 2048, true)) {
      g_shadow_cfg = -1;
      g_shadow_rate = 0;
      return;  // not a benchmark rate (or no memory): skip silently
    }
    g_ring_n.store(0);
  }
  const uint32_t t0 = cycles();
  g_shadow_fe->process(cu8, bytes / 2, g_shadow_out, 2048);
  g_shadow_cyc = cycles() - t0;
  g_shadow_pending = true;
}

void shadow_total(uint32_t block_us) {
  if (!g_shadow_pending || !g_ring_fe) return;
  const uint32_t k = g_ring_n.load(std::memory_order_relaxed);
  const uint32_t fe_us = static_cast<uint32_t>(g_shadow_cyc / (kCpuHz / 1e6));
  g_ring_fe[k % kRing] = static_cast<uint16_t>(std::min<uint32_t>(fe_us, 65535));
  g_ring_total[k % kRing] = static_cast<uint16_t>(std::min<uint32_t>(block_us, 65535));
  g_ring_n.store(k + 1, std::memory_order_relaxed);
  g_shadow_pending = false;
}

}  // namespace orcsdr::dsp::lab
