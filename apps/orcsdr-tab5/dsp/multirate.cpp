#include "multirate.hpp"

#include <algorithm>
#include <cstring>

#include <esp_cpu.h>
#include <esp_heap_caps.h>

#include "dsps_fir.h"
#include "frontend_coeffs.hpp"

namespace orcsdr::dsp::mr {
namespace {

namespace co = orcsdr::dsp::coeffs;

constexpr int kHb1Taps = static_cast<int>(sizeof(co::kHb1Q15) / sizeof(co::kHb1Q15[0]));
constexpr int kHb2Taps = static_cast<int>(sizeof(co::kHb2Q15) / sizeof(co::kHb2Q15[0]));
constexpr int kHb3Taps = static_cast<int>(sizeof(co::kHb3Q15) / sizeof(co::kHb3Q15[0]));
constexpr int kH1 = kHb1Taps - 1;
constexpr int kH2 = kHb2Taps - 1;
constexpr int kHb3Hist = kHb3Taps - 1;
static_assert(kHb1Taps % 4 == 3 && kHb2Taps % 4 == 3 && kHb3Taps % 4 == 3,
              "halfband length must be 4k+3");
static_assert(kH1 <= 8, "hb1 history buffer");

// Runtime copies: the compiler must not see the values (a constant-folded
// full-tap kernel would silently become the sparse one).
int16_t g_hb1_q[kHb1Taps];
int16_t g_hb2_q[kHb2Taps];
float g_hb1_f[kHb1Taps];
float g_hb2_f[kHb2Taps];
int32_t g_hb1_bias = 0;  // folds the -128 CU8 centering and the >>8 rounding
bool g_coeffs_ready = false;

void load_coeffs() {
  if (g_coeffs_ready) return;
  int32_t sum = 0;
  for (int k = 0; k < kHb1Taps; ++k) {
    g_hb1_q[k] = co::kHb1Q15[k];
    g_hb1_f[k] = co::kHb1F[k];
    sum += co::kHb1Q15[k];
  }
  // acc = sum h*(u-128) + 128 = sum h*u - 128*sum h + 128
  g_hb1_bias = 128 - 128 * sum;
  for (int k = 0; k < kHb2Taps; ++k) {
    g_hb2_q[k] = co::kHb2Q15[k];
    g_hb2_f[k] = co::kHb2F[k];
  }
  g_coeffs_ready = true;
}

inline uint32_t cycles() { return esp_cpu_get_cycle_count(); }

// ---- HB1: CU8 -> stage (window ends at p, p[-kH1..0] valid) --------------

// B: all taps, raw u with the centering folded into the bias.
inline void hb1_full(const Cu8* p, Cs16& y) {
  int32_t ai = g_hb1_bias, aq = g_hb1_bias;
  for (int k = 0; k < kHb1Taps; ++k) {
    ai += g_hb1_q[k] * static_cast<int32_t>(p[-k].i);
    aq += g_hb1_q[k] * static_cast<int32_t>(p[-k].q);
  }
  y.i = static_cast<int16_t>(ai >> 8);
  y.q = static_cast<int16_t>(aq >> 8);
}

// D: center tap + folded symmetric odd pairs (zero taps never touched).
inline void hb1_sparse(const Cu8* p, Cs16& y) {
  constexpr int c = kHb1Taps / 2;
  int32_t ai = g_hb1_bias + g_hb1_q[c] * static_cast<int32_t>(p[-c].i);
  int32_t aq = g_hb1_bias + g_hb1_q[c] * static_cast<int32_t>(p[-c].q);
  for (int j = 1; j <= c; j += 2) {
    const int32_t h = g_hb1_q[c + j];
    ai += h * (static_cast<int32_t>(p[-c - j].i) + p[-c + j].i);
    aq += h * (static_cast<int32_t>(p[-c - j].q) + p[-c + j].q);
  }
  y.i = static_cast<int16_t>(ai >> 8);
  y.q = static_cast<int16_t>(aq >> 8);
}

inline void hb1_specialized(const Cu8* p, Cs16& y) {
  constexpr int c = kHb1Taps / 2;
  static_assert(co::kHb1Q15[0] * 2 + co::kHb1Q15[2] * 2 + co::kHb1Q15[3] == 32768);
  int32_t ai = (128 - 128 * 32768) + co::kHb1Q15[c] * static_cast<int32_t>(p[-c].i);
  int32_t aq = (128 - 128 * 32768) + co::kHb1Q15[c] * static_cast<int32_t>(p[-c].q);
#pragma GCC unroll 2
  for (int j = 1; j <= c; j += 2) {
    const int32_t h = co::kHb1Q15[c + j];
    ai += h * (static_cast<int32_t>(p[-c - j].i) + p[-c + j].i);
    aq += h * (static_cast<int32_t>(p[-c - j].q) + p[-c + j].q);
  }
  y.i = static_cast<int16_t>(ai >> 8);
  y.q = static_cast<int16_t>(aq >> 8);
}

// A: float, all taps.
inline void hb1_float(const Cu8* p, Cf32& y) {
  float ai = 0.0f, aq = 0.0f;
  for (int k = 0; k < kHb1Taps; ++k) {
    ai += g_hb1_f[k] * static_cast<float>(static_cast<int32_t>(p[-k].i) - 128);
    aq += g_hb1_f[k] * static_cast<float>(static_cast<int32_t>(p[-k].q) - 128);
  }
  y.i = ai;
  y.q = aq;
}

// Runs a decimate-by-2 kernel over CU8 with a stitched history.
template <class Out, class Kern>
size_t hb1_run(const Cu8* x, size_t n, Cu8* hist, uint8_t& phase, Out* y, Kern kern) {
  Cu8 tmp[2 * kH1];
  memcpy(tmp, hist, sizeof(Cu8) * kH1);
  const size_t m = std::min<size_t>(n, kH1);
  memcpy(tmp + kH1, x, sizeof(Cu8) * m);
  size_t out = 0;
  size_t b = phase;
  for (; b < m; b += 2) kern(tmp + kH1 + b, y[out++]);
  for (; b < n; b += 2) kern(x + b, y[out++]);
  phase = static_cast<uint8_t>(b - n);
  if (n >= static_cast<size_t>(kH1)) {
    memcpy(hist, x + n - kH1, sizeof(Cu8) * kH1);
  } else {
    memmove(hist, hist + n, sizeof(Cu8) * (kH1 - n));
    memcpy(hist + kH1 - n, x, sizeof(Cu8) * n);
  }
  return out;
}

// ---- HB2: stage -> stage over a history-prefixed buffer ------------------

inline void hb2_full(const Cs16* p, int32_t& ai, int32_t& aq) {
  ai = 1 << 14;
  aq = 1 << 14;
  for (int k = 0; k < kHb2Taps; ++k) {
    ai += g_hb2_q[k] * static_cast<int32_t>(p[-k].i);
    aq += g_hb2_q[k] * static_cast<int32_t>(p[-k].q);
  }
}

inline void hb2_sparse(const Cs16* p, int32_t& ai, int32_t& aq) {
  constexpr int c = kHb2Taps / 2;
  ai = (1 << 14) + g_hb2_q[c] * static_cast<int32_t>(p[-c].i);
  aq = (1 << 14) + g_hb2_q[c] * static_cast<int32_t>(p[-c].q);
  for (int j = 1; j <= c; j += 2) {
    const int32_t h = g_hb2_q[c + j];
    ai += h * (static_cast<int32_t>(p[-c - j].i) + p[-c + j].i);
    aq += h * (static_cast<int32_t>(p[-c - j].q) + p[-c + j].q);
  }
}

inline void hb2_specialized(const Cs16* p, int32_t& ai, int32_t& aq) {
  constexpr int c = kHb2Taps / 2;
  ai = (1 << 14) + co::kHb2Q15[c] * static_cast<int32_t>(p[-c].i);
  aq = (1 << 14) + co::kHb2Q15[c] * static_cast<int32_t>(p[-c].q);
#pragma GCC unroll 4
  for (int j = 1; j <= c; j += 2) {
    const int32_t h = co::kHb2Q15[c + j];
    ai += h * (static_cast<int32_t>(p[-c - j].i) + p[-c + j].i);
    aq += h * (static_cast<int32_t>(p[-c - j].q) + p[-c + j].q);
  }
}

inline void hb2_float(const Cf32* p, Cf32& y) {
  float ai = 0.0f, aq = 0.0f;
  for (int k = 0; k < kHb2Taps; ++k) {
    ai += g_hb2_f[k] * p[-k].i;
    aq += g_hb2_f[k] * p[-k].q;
  }
  y.i = ai;
  y.q = aq;
}

// comb = [kH2 history | n new]; kern(window_end, out_index).
template <class T, class Kern>
size_t hb2_run(T* comb, size_t n, uint8_t& phase, Kern kern) {
  size_t out = 0;
  size_t b = phase;
  for (; b < n; b += 2) kern(comb + kH2 + b, out++);
  phase = static_cast<uint8_t>(b - n);
  memmove(comb, comb + n, sizeof(T) * kH2);
  return out;
}

inline void hb3_sparse(const Cs16* p, Cs16& y) {
  constexpr int c = kHb3Taps / 2;
  int32_t ai = 1 << 15, aq = 1 << 15;
  ai += co::kHb3Q15[c] * static_cast<int32_t>(p[-c].i);
  aq += co::kHb3Q15[c] * static_cast<int32_t>(p[-c].q);
  for (int j = 1; j <= c; j += 2) {
    const int32_t h = co::kHb3Q15[c + j];
    ai += h * (static_cast<int32_t>(p[-c - j].i) + p[-c + j].i);
    aq += h * (static_cast<int32_t>(p[-c - j].q) + p[-c + j].q);
  }
  // Half-scale Q6: the any-input bound would exceed int16 at Q7.
  y.i = static_cast<int16_t>(ai >> 16);
  y.q = static_cast<int16_t>(aq >> 16);
}

size_t hb3_run(Cs16* comb, size_t n, uint8_t& phase, Cs16* out) {
  size_t o = 0;
  size_t b = phase;
  for (; b < n; b += 2) hb3_sparse(comb + kHb3Hist + b, out[o++]);
  phase = static_cast<uint8_t>(b - n);
  memmove(comb, comb + n, sizeof(Cs16) * kHb3Hist);
  return o;
}

// ---- P4 PIE dot products (candidates E1/E2) ---------------------------------
// Coefficient vectors live in q1..qN (loaded by the caller); data streams
// through q0 with the fused multiply-accumulate + load. xacc is preloaded with
// the rounding bias and shifted with esp.srs.s.xacc, which reproduces D's
// (acc + bias) >> s exactly. Fully unrolled: no loops, no esp.lp.setup.
#define ORC_PIE_HEAD "esp.zero.xacc\n esp.movx.w.xacc.l %[pre]\n esp.vld.128.ip q0, %[x], 16\n"
#define ORC_PIE_MLD(k) "esp.vmulas.s16.xacc.ld.ip q0, %[x], 16, q0, q" #k "\n"
#define ORC_PIE_TAIL(k) "esp.vmulas.s16.xacc q0, q" #k "\n esp.srs.s.xacc %[r], %[sh]\n"
// PIE instructions only accept GPRs x24-x31 (s8-s11, t3-t6) in these operand
// slots (the assembler rejects others), so every operand is pinned to one.
#define ORC_PIE_DOT(name, body)                                              \
  inline int32_t name(const int16_t* x_, int32_t pre_, int32_t sh_) {        \
    register const int16_t* x asm("s9") = x_;                                \
    register int32_t pre asm("t3") = pre_;                                   \
    register int32_t sh asm("t4") = sh_;                                     \
    register int32_t r asm("t5");                                            \
    asm volatile(body                                                        \
                 : [x] "+r"(x), [r] "=r"(r)                                  \
                 : [pre] "r"(pre), [sh] "r"(sh)                              \
                 : "memory");                                                \
    return r;                                                                \
  }
ORC_PIE_DOT(pie_dot1, ORC_PIE_HEAD ORC_PIE_TAIL(1))
ORC_PIE_DOT(pie_dot2, ORC_PIE_HEAD ORC_PIE_MLD(1) ORC_PIE_TAIL(2))
ORC_PIE_DOT(pie_dot6, ORC_PIE_HEAD ORC_PIE_MLD(1) ORC_PIE_MLD(2) ORC_PIE_MLD(3) ORC_PIE_MLD(4)
                          ORC_PIE_MLD(5) ORC_PIE_TAIL(6))
ORC_PIE_DOT(pie_dot7, ORC_PIE_HEAD ORC_PIE_MLD(1) ORC_PIE_MLD(2) ORC_PIE_MLD(3) ORC_PIE_MLD(4)
                          ORC_PIE_MLD(5) ORC_PIE_MLD(6) ORC_PIE_TAIL(7))

inline void pie_load_coef(const int16_t* h_, int n) {
  // q1..qn <- h[0..8n)
  register const int16_t* h asm("s10") = h_;
  switch (n) {
    case 7: asm volatile("esp.vld.128.ip q1, %0, 16\n esp.vld.128.ip q2, %0, 16\n"
                         "esp.vld.128.ip q3, %0, 16\n esp.vld.128.ip q4, %0, 16\n"
                         "esp.vld.128.ip q5, %0, 16\n esp.vld.128.ip q6, %0, 16\n"
                         "esp.vld.128.ip q7, %0, 16\n" : "+r"(h) :: "memory"); break;
    case 6: asm volatile("esp.vld.128.ip q1, %0, 16\n esp.vld.128.ip q2, %0, 16\n"
                         "esp.vld.128.ip q3, %0, 16\n esp.vld.128.ip q4, %0, 16\n"
                         "esp.vld.128.ip q5, %0, 16\n esp.vld.128.ip q6, %0, 16\n"
                         : "+r"(h) :: "memory"); break;
    case 2: asm volatile("esp.vld.128.ip q1, %0, 16\n esp.vld.128.ip q2, %0, 16\n"
                         : "+r"(h) :: "memory"); break;
    default: asm volatile("esp.vld.128.ip q1, %0, 16\n" : "+r"(h) :: "memory"); break;
  }
}

struct PieUnaligned {  // enable unaligned 128-bit loads for a scope
  uint32_t old;
  PieUnaligned() {
    register uint32_t v asm("t6");
    asm volatile("esp.movx.r.cfg %0" : "=r"(v));
    old = v;
    register uint32_t w asm("t6") = old | 2u;
    asm volatile("esp.movx.w.cfg %0" ::"r"(w));
  }
  ~PieUnaligned() {
    register uint32_t w asm("t6") = old;
    asm volatile("esp.movx.w.cfg %0" ::"r"(w));
  }
};

constexpr float kQ15ToC = 1.0f / 128.0f;           // stage value -> c units
constexpr float kPpQ15ToC = 1.0f / (32768.0f * 128.0f);

template <const int16_t* Proto, int L, int P, int Phase>
inline Cf32 d2_dot(const Cs16* xw) {
  int32_t ai0 = 0, aq0 = 0, ai1 = 0, aq1 = 0;
  int j = 0;
#pragma GCC unroll 4
  for (; j + 1 < P; j += 2) {
    const int32_t h0 = Proto[Phase + (P - 1 - j) * L];
    const int32_t h1 = Proto[Phase + (P - 2 - j) * L];
    ai0 += h0 * static_cast<int32_t>(xw[j].i);
    aq0 += h0 * static_cast<int32_t>(xw[j].q);
    ai1 += h1 * static_cast<int32_t>(xw[j + 1].i);
    aq1 += h1 * static_cast<int32_t>(xw[j + 1].q);
  }
  if (j < P) {
    const int32_t h = Proto[Phase];
    ai0 += h * static_cast<int32_t>(xw[j].i);
    aq0 += h * static_cast<int32_t>(xw[j].q);
  }
  return {static_cast<float>(ai0 + ai1) * kPpQ15ToC,
          static_cast<float>(aq0 + aq1) * kPpQ15ToC};
}

template <const int16_t* Proto, int L, int M, int P>
size_t d2_poly(const Cs16* comb, uint32_t& i, uint16_t& phase, uint32_t end, Cf32* out) {
  size_t o = 0;
  while (i < end) {
    const Cs16* xw = comb + (i - (P - 1));
    if constexpr (L == 1) {
      out[o++] = d2_dot<Proto, L, P, 0>(xw);
      i += 3;
    } else if constexpr (L == 2) {
      if (phase == 0) {
        out[o++] = d2_dot<Proto, L, P, 0>(xw);
        i += 2; phase = 1;
      } else {
        out[o++] = d2_dot<Proto, L, P, 1>(xw);
        i += 3; phase = 0;
      }
    } else if constexpr (M == 8) {
      if (phase == 0) {
        out[o++] = d2_dot<Proto, L, P, 0>(xw);
        i += 2; phase = 2;
      } else if (phase == 2) {
        out[o++] = d2_dot<Proto, L, P, 2>(xw);
        i += 3; phase = 1;
      } else {
        out[o++] = d2_dot<Proto, L, P, 1>(xw);
        i += 3; phase = 0;
      }
    } else {
      static_assert(L == 3 && M == 10);
      if (phase == 0) {
        out[o++] = d2_dot<Proto, L, P, 0>(xw);
        i += 3; phase = 1;
      } else if (phase == 1) {
        out[o++] = d2_dot<Proto, L, P, 1>(xw);
        i += 3; phase = 2;
      } else {
        out[o++] = d2_dot<Proto, L, P, 2>(xw);
        i += 4; phase = 0;
      }
    }
  }
  return o;
}

template <const int16_t* Proto, int L, int M, int P>
size_t d3_poly(const Cs16* comb, uint32_t& i, uint16_t& phase, uint32_t end, Cf32* out) {
  size_t o = 0;
  while (i < end) {
    const Cs16* xw = comb + (i - (P - 1));
    switch (phase) {
      case 0: out[o] = d2_dot<Proto, L, P, 0>(xw); break;
      case 1: out[o] = d2_dot<Proto, L, P, 1>(xw); break;
      default:
        if constexpr (L >= 3) {
          if (phase == 2) out[o] = d2_dot<Proto, L, P, 2>(xw);
          else if constexpr (L == 4) out[o] = d2_dot<Proto, L, P, 3>(xw);
        }
    }
    out[o].i *= 2.0f;  // HB3 stored at half-scale.
    out[o].q *= 2.0f;
    ++o;
    const uint16_t next = phase + M;
    i += next / L;
    phase = next % L;
  }
  return o;
}

}  // namespace

const char* candidate_name(Candidate c) {
  switch (c) {
    case Candidate::a_float: return "A_float";
    case Candidate::b_q15_full: return "B_q15_full";
    case Candidate::c_espdsp_ansi: return "C_espdsp_ansi";
    case Candidate::c_espdsp_arp4: return "C_espdsp_arp4";
    case Candidate::d_q15_sparse: return "D_q15_sparse";
    case Candidate::d2_q15_specialized: return "D2_q15_specialized";
    case Candidate::d3_q15_div8: return "D3_q15_div8";
    case Candidate::e_pie_pp: return "E1_pie_pp";
    case Candidate::e_pie_all: return "E2_pie_all";
    default: return "?";
  }
}

const char* pp_name(PpKind p) { return p == PpKind::q15 ? "pp_q15" : "pp_f32"; }

Frontend::~Frontend() { release(); }

void* Frontend::alloc(size_t bytes) {
  const uint32_t caps = internal_ ? (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
                                  : (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  void* p = heap_caps_aligned_alloc(16, bytes, caps);
  if (p == nullptr) return nullptr;
  memset(p, 0, bytes);
  if (internal_) mem_.internal += bytes;
  else mem_.psram += bytes;
  if (nblocks_ < static_cast<int>(sizeof(blocks_) / sizeof(blocks_[0]))) blocks_[nblocks_++] = p;
  return p;
}

void Frontend::release() {
  if (c_fir_ != nullptr) {
    auto* fir = static_cast<fir_s16_t*>(c_fir_);
    for (int k = 0; k < 4; ++k) dsps_fird_s16_aexx_free(&fir[k]);
  }
  for (int k = 0; k < nblocks_; ++k) heap_caps_free(blocks_[k]);
  nblocks_ = 0;
  mem_ = {};
  hb2_buf_ = hb3_buf_ = pp_buf_ = c_fir_ = nullptr;
  c_i0_ = c_q0_ = c_i1_ = c_q1_ = c_i2_ = c_q2_ = nullptr;
  c_hb1_coeffs_ = c_hb2_coeffs_ = nullptr;
  pp_f_ = nullptr;
  pp_q_ = nullptr;
  pp_adv_ = nullptr;
  e_i_ = e_q_ = e_coef_ = nullptr;
  e1i_ = e1q_ = e2i_ = e2q_ = e_hb1c_ = e_hb2c_ = nullptr;
  device_rate_ = 0;
  arp4_active_ = false;
  c_pending_n_ = 0;
  state_ = InitState::uninitialized;
}

const void* Frontend::espdsp_fir(int k) const {
  if (c_fir_ == nullptr || k < 0 || k > 3) return nullptr;
  return static_cast<const fir_s16_t*>(c_fir_) + k;
}

bool Frontend::init(uint32_t device_rate, Candidate cand, PpKind pp, size_t chunk_in,
                    bool internal_buffers) {
  const bool ok = init_impl(device_rate, cand, pp, chunk_in, internal_buffers);
  if (!ok) {
    release();
    state_ = InitState::failed;
    return false;
  }
  reset();
  state_ = InitState::ready;
  return true;
}

bool Frontend::init_impl(uint32_t device_rate, Candidate cand, PpKind pp, size_t chunk_in,
                         bool internal_buffers) {
  release();
  load_coeffs();
  const co::PpPlan* plan = nullptr;
  if (cand == Candidate::d3_q15_div8) {
    for (const auto& p : co::kDiv8Plans)
      if (p.device_rate == device_rate) plan = &p;
  } else {
    for (const auto& p : co::kPlans)
      if (p.device_rate == device_rate) plan = &p;
  }
  if (plan == nullptr || chunk_in == 0 || (chunk_in & 3) != 0) return false;
  if (cand == Candidate::a_float) pp = PpKind::f32;  // A is float end to end
  if (cand == Candidate::e_pie_pp || cand == Candidate::e_pie_all ||
      cand == Candidate::d2_q15_specialized || cand == Candidate::d3_q15_div8)
    pp = PpKind::q15;  // D2/E are Q15 end to end
  device_rate_ = device_rate;
  cand_ = cand;
  pp_ = pp;
  chunk_in_ = chunk_in;
  internal_ = internal_buffers;
  L_ = plan->L;
  M_ = plan->M;
  taps_ = plan->taps;
  P_ = static_cast<uint16_t>(taps_ / L_);

  const bool float_stage = cand == Candidate::a_float;
  const size_t n1 = chunk_in / 2, n2 = chunk_in / 4;
  hb2_buf_ = alloc((kH2 + n1) * (float_stage ? sizeof(Cf32) : sizeof(Cs16)));
  if (cand == Candidate::d3_q15_div8) {
    hb3_buf_ = alloc((kHb3Hist + n2) * sizeof(Cs16));
    if (!hb3_buf_) return false;
  }
  const bool pp_float = pp_ == PpKind::f32;
  if (cand == Candidate::e_pie_pp || cand == Candidate::e_pie_all) {
    P8_ = static_cast<uint16_t>((P_ + 7) & ~7);
    if (P8_ != 48 && P8_ != 56) return false;  // unrolled for 6 or 7 vectors
    e_i_ = static_cast<int16_t*>(alloc(sizeof(int16_t) * (P_ - 1 + n2 + 8)));
    e_q_ = static_cast<int16_t*>(alloc(sizeof(int16_t) * (P_ - 1 + n2 + 8)));
    e_coef_ = static_cast<int16_t*>(alloc(sizeof(int16_t) * L_ * P8_));
    if (hb2_buf_ == nullptr || !e_i_ || !e_q_ || !e_coef_) return false;
    for (int p = 0; p < L_; ++p)
      for (int j = 0; j < P_; ++j) e_coef_[p * P8_ + j] = plan->proto_q15[p + (P_ - 1 - j) * L_];
    if (cand == Candidate::e_pie_all) {
      e1i_ = static_cast<int16_t*>(alloc(sizeof(int16_t) * (kH1 + chunk_in + 8)));
      e1q_ = static_cast<int16_t*>(alloc(sizeof(int16_t) * (kH1 + chunk_in + 8)));
      e2i_ = static_cast<int16_t*>(alloc(sizeof(int16_t) * (kH2 + n1 + 8)));
      e2q_ = static_cast<int16_t*>(alloc(sizeof(int16_t) * (kH2 + n1 + 8)));
      e_hb1c_ = static_cast<int16_t*>(alloc(sizeof(int16_t) * 8));
      e_hb2c_ = static_cast<int16_t*>(alloc(sizeof(int16_t) * 16));
      if (!e1i_ || !e1q_ || !e2i_ || !e2q_ || !e_hb1c_ || !e_hb2c_) return false;
      static_assert(kHb1Taps == 7 && kHb2Taps == 15, "E2 is laid out for 8/16-lane halfbands");
      // Window x[t-6..t] (resp. x[t-14..t]) starts at combined index b; lane
      // k holds h[N-1-k] and the last lane is a zero pad (reads one spare).
      for (int k = 0; k < kHb1Taps; ++k) e_hb1c_[k] = co::kHb1Q15[kHb1Taps - 1 - k];
      for (int k = 0; k < kHb2Taps; ++k) e_hb2c_[k] = co::kHb2Q15[kHb2Taps - 1 - k];
    }
  } else {
    pp_buf_ = alloc((P_ - 1 + (cand == Candidate::d3_q15_div8 ? (n2 + 1) / 2 : n2)) *
                    (pp_float ? sizeof(Cf32) : sizeof(Cs16)));
    if (hb2_buf_ == nullptr || pp_buf_ == nullptr) return false;
  }

  // Per-phase reversed coefficient tables: y = sum_j hp[p][j] x[i-P+1+j],
  // hp[p][j] = h[p + (P-1-j) L].
  if (cand == Candidate::d2_q15_specialized || cand == Candidate::d3_q15_div8) {
    // D2/D3 read generated Q15 coefficients directly from flash.
  } else if (pp_float) {
    pp_f_ = static_cast<float*>(alloc(sizeof(float) * L_ * P_));
    if (pp_f_ == nullptr) return false;
  } else {
    pp_q_ = static_cast<int16_t*>(alloc(sizeof(int16_t) * L_ * P_));
    if (pp_q_ == nullptr) return false;
  }
  if (cand != Candidate::d2_q15_specialized && cand != Candidate::d3_q15_div8)
    for (int p = 0; p < L_; ++p)
      for (int j = 0; j < P_; ++j) {
        const int k = p + (P_ - 1 - j) * L_;
        if (pp_float) pp_f_[p * P_ + j] = plan->proto_f[k];
        else pp_q_[p * P_ + j] = plan->proto_q15[k];
      }
  if (cand != Candidate::d2_q15_specialized && cand != Candidate::d3_q15_div8) {
    pp_adv_ = static_cast<uint16_t*>(alloc(sizeof(uint16_t) * 2 * L_));
    if (pp_adv_ == nullptr) return false;
    for (int p = 0; p < L_; ++p) {
      pp_adv_[2 * p] = static_cast<uint16_t>((p + M_) / L_);
      pp_adv_[2 * p + 1] = static_cast<uint16_t>((p + M_) % L_);
    }
  }

  if (cand == Candidate::c_espdsp_ansi || cand == Candidate::c_espdsp_arp4) {
    c_i0_ = static_cast<int16_t*>(alloc(sizeof(int16_t) * chunk_in));
    c_q0_ = static_cast<int16_t*>(alloc(sizeof(int16_t) * chunk_in));
    c_i1_ = static_cast<int16_t*>(alloc(sizeof(int16_t) * n1));
    c_q1_ = static_cast<int16_t*>(alloc(sizeof(int16_t) * n1));
    c_i2_ = static_cast<int16_t*>(alloc(sizeof(int16_t) * n2));
    c_q2_ = static_cast<int16_t*>(alloc(sizeof(int16_t) * n2));
    // Pad to a multiple of 8 (the arp4 kernel silently falls back to ANSI
    // otherwise) with a leading zero: that delays the window by one input,
    // so with start_pos 0 the outputs line up exactly with ours.
    const int len1 = (kHb1Taps + 7) & ~7, len2 = (kHb2Taps + 7) & ~7;
    c_hb1_coeffs_ = static_cast<int16_t*>(alloc(sizeof(int16_t) * len1));
    c_hb2_coeffs_ = static_cast<int16_t*>(alloc(sizeof(int16_t) * len2));
    c_fir_ = alloc(sizeof(fir_s16_t) * 4);
    if (!c_i0_ || !c_q0_ || !c_i1_ || !c_q1_ || !c_i2_ || !c_q2_ || !c_hb1_coeffs_ ||
        !c_hb2_coeffs_ || !c_fir_)
      return false;
    for (int k = 0; k < kHb1Taps; ++k) c_hb1_coeffs_[len1 - kHb1Taps + k] = co::kHb1Q15[k];
    for (int k = 0; k < kHb2Taps; ++k) c_hb2_coeffs_[len2 - kHb2Taps + k] = co::kHb2Q15[k];
    if (cand == Candidate::c_espdsp_arp4) {
      // dsps_fird_s16_arp4 pairs coeffs[0] with the OLDEST sample (ANSI pairs
      // it with the newest), i.e. it expects the reversed array. Undocumented
      // for P4; the lab's ARP4 unit test shows bit-identical output this way.
      std::reverse(c_hb1_coeffs_, c_hb1_coeffs_ + len1);
      std::reverse(c_hb2_coeffs_, c_hb2_coeffs_ + len2);
    }
    auto* fir = static_cast<fir_s16_t*>(c_fir_);
    // ESP-DSP 1.8.2's optimized init dereferences its own memalign results
    // without checking them. Refuse a lab candidate under memory pressure.
    constexpr uint32_t kFirHeadroom = 8192;
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < kFirHeadroom ||
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 128)
      return false;
    // HB1: acc = sum h*c (c unshifted) -> >>8 gives c*128: shift - 15 = -8.
    // HB2: stage in, stage out: >>15, shift 0.
    for (int k = 0; k < 2; ++k)
      if (dsps_fird_init_s16(&fir[k], c_hb1_coeffs_, nullptr, len1, 2, 0, 7) != ESP_OK)
        return false;
    for (int k = 2; k < 4; ++k)
      if (dsps_fird_init_s16(&fir[k], c_hb2_coeffs_, nullptr, len2, 2, 0, 0) != ESP_OK)
        return false;
    // The library allocates delay lines itself; count them (approximate).
    mem_.internal += 4 * ((len2 + 16) * sizeof(int16_t) + 8);
    arp4_active_ = cand == Candidate::c_espdsp_arp4 && (len1 % 8) == 0 && (len2 % 8) == 0;
  }
  return true;
}

void Frontend::reset() {
  memset(hb1_hist_, 128, sizeof(hb1_hist_));  // CU8 zero is 128
  memset(hb1_hist_f_, 0, sizeof(hb1_hist_f_));
  hb1_phase_ = 0;
  hb2_phase_ = 0;
  hb3_phase_ = 0;
  c_pending_n_ = 0;
  const bool float_stage = cand_ == Candidate::a_float;
  if (hb2_buf_) memset(hb2_buf_, 0, kH2 * (float_stage ? sizeof(Cf32) : sizeof(Cs16)));
  if (hb3_buf_) memset(hb3_buf_, 0, kHb3Hist * sizeof(Cs16));
  if (pp_buf_)
    memset(pp_buf_, 0, (P_ - 1) * (pp_ == PpKind::f32 ? sizeof(Cf32) : sizeof(Cs16)));
  if (e_i_) memset(e_i_, 0, sizeof(int16_t) * (P_ - 1));
  if (e1i_) memset(e1i_, 0, sizeof(int16_t) * kH1);
  if (e1q_) memset(e1q_, 0, sizeof(int16_t) * kH1);
  if (e2i_) memset(e2i_, 0, sizeof(int16_t) * kH2);
  if (e2q_) memset(e2q_, 0, sizeof(int16_t) * kH2);
  if (e_q_) memset(e_q_, 0, sizeof(int16_t) * (P_ - 1));
  pp_i_ = P_ - 1;
  pp_p_ = 0;
  peak_q15_ = 0;
  if (c_fir_ != nullptr) {
    auto* fir = static_cast<fir_s16_t*>(c_fir_);
    for (int k = 0; k < 4; ++k) {
      memset(fir[k].delay, 0, sizeof(int16_t) * fir[k].coeffs_len);
      fir[k].pos = 0;
      fir[k].d_pos = 0;
    }
  }
}

size_t Frontend::max_out(size_t n) const {
  // Outputs per input = L / (4 M), or L / (8 M) for D3, plus phase carry.
  const size_t chunks = (n + chunk_in_ - 1) / chunk_in_ + 1;
  const size_t coarse = cand_ == Candidate::d3_q15_div8 ? 8 : 4;
  return (n / coarse + chunks * 2) * L_ / M_ + chunks + 2;
}

size_t Frontend::process(const uint8_t* cu8, size_t n, Cf32* out, size_t out_cap,
                         StageCycles* cycles_out) {
  size_t produced = 0;
  if (state_ != InitState::ready || (n && (!cu8 || !out))) return 0;
  if (out_cap < max_out(n + c_pending_n_)) return 0;
  if (cand_ == Candidate::c_espdsp_ansi || cand_ == Candidate::c_espdsp_arp4) {
    if (c_pending_n_) {
      const size_t take = std::min(n, static_cast<size_t>(4 - c_pending_n_));
      memcpy(c_pending_ + c_pending_n_, cu8, take * sizeof(Cu8));
      c_pending_n_ += static_cast<uint8_t>(take);
      cu8 += take * sizeof(Cu8);
      n -= take;
      if (c_pending_n_ == 4) {
        produced += chunk(reinterpret_cast<const uint8_t*>(c_pending_), 4,
                          out + produced, cycles_out);
        c_pending_n_ = 0;
      }
    }
    while (n >= 4) {
      const size_t take = std::min(n & ~size_t{3}, chunk_in_);
      produced += chunk(cu8, take, out + produced, cycles_out);
      cu8 += take * sizeof(Cu8);
      n -= take;
    }
    if (n) {
      memcpy(c_pending_, cu8, n * sizeof(Cu8));
      c_pending_n_ = static_cast<uint8_t>(n);
    }
    return produced;
  }
  while (n > 0) {
    const size_t take = std::min(n, chunk_in_);
    produced += chunk(cu8, take, out + produced, cycles_out);
    cu8 += take * 2;
    n -= take;
  }
  return produced;
}

size_t Frontend::chunk(const uint8_t* cu8, size_t n, Cf32* out, StageCycles* cyc) {
  const auto* x = reinterpret_cast<const Cu8*>(cu8);
  const uint32_t H3 = P_ - 1u;
  uint32_t t0 = cycles();
  size_t n2 = 0;  // samples written into the PP buffer (after its history)

  if (cand_ == Candidate::a_float) {
    auto* comb2 = static_cast<Cf32*>(hb2_buf_);
    const size_t n1 = hb1_run(x, n, hb1_hist_, hb1_phase_, comb2 + kH2,
                              [](const Cu8* p, Cf32& y) { hb1_float(p, y); });
    uint32_t t1 = cycles();
    auto* pp_in = static_cast<Cf32*>(pp_buf_) + H3;
    n2 = hb2_run(comb2, n1, hb2_phase_,
                 [pp_in](const Cf32* p, size_t o) { hb2_float(p, pp_in[o]); });
    uint32_t t2 = cycles();
    if (cyc) { cyc->hb1 += t1 - t0; cyc->hb2 += t2 - t1; }
    t0 = t2;
  } else if (cand_ == Candidate::e_pie_all) {
    const PieUnaligned unaligned;
    int16_t* ci = e1i_ + kH1;
    int16_t* cq = e1q_ + kH1;
    for (size_t k = 0; k < n; ++k) {  // de-interleave + center (c = u - 128)
      ci[k] = static_cast<int16_t>(static_cast<int32_t>(x[k].i) - 128);
      cq[k] = static_cast<int16_t>(static_cast<int32_t>(x[k].q) - 128);
    }
    uint32_t tc = cycles();
    // HB1: window starts at combined index b; (acc + 128) >> 8.
    pie_load_coef(e_hb1c_, 1);
    size_t n1 = 0;
    size_t b = hb1_phase_;
    int16_t* hi = e2i_ + kH2;
    int16_t* hq = e2q_ + kH2;
    for (; b < n; b += 2, ++n1) {
      hi[n1] = static_cast<int16_t>(pie_dot1(e1i_ + b, 128, 8));
      hq[n1] = static_cast<int16_t>(pie_dot1(e1q_ + b, 128, 8));
    }
    hb1_phase_ = static_cast<uint8_t>(b - n);
    memmove(e1i_, e1i_ + n, sizeof(int16_t) * kH1);
    memmove(e1q_, e1q_ + n, sizeof(int16_t) * kH1);
    uint32_t t1 = cycles();
    // HB2: window starts at combined index b; (acc + 2^14) >> 15.
    pie_load_coef(e_hb2c_, 2);
    int16_t* ei = e_i_ + H3;
    int16_t* eq = e_q_ + H3;
    b = hb2_phase_;
    for (; b < n1; b += 2, ++n2) {
      ei[n2] = static_cast<int16_t>(pie_dot2(e2i_ + b, 1 << 14, 15));
      eq[n2] = static_cast<int16_t>(pie_dot2(e2q_ + b, 1 << 14, 15));
    }
    hb2_phase_ = static_cast<uint8_t>(b - n1);
    memmove(e2i_, e2i_ + n1, sizeof(int16_t) * kH2);
    memmove(e2q_, e2q_ + n1, sizeof(int16_t) * kH2);
    uint32_t t2 = cycles();
    if (cyc) { cyc->other += tc - t0; cyc->hb1 += t1 - tc; cyc->hb2 += t2 - t1; }
    t0 = t2;
  } else if (cand_ == Candidate::e_pie_pp) {
    auto* comb2 = static_cast<Cs16*>(hb2_buf_);
    const size_t n1 = hb1_run(x, n, hb1_hist_, hb1_phase_, comb2 + kH2,
                              [](const Cu8* p, Cs16& y) { hb1_sparse(p, y); });
    uint32_t t1 = cycles();
    int16_t* ei = e_i_ + H3;
    int16_t* eq = e_q_ + H3;
    n2 = hb2_run(comb2, n1, hb2_phase_, [ei, eq](const Cs16* p, size_t o) {
      int32_t ai, aq;
      hb2_sparse(p, ai, aq);
      ei[o] = static_cast<int16_t>(ai >> 15);
      eq[o] = static_cast<int16_t>(aq >> 15);
    });
    uint32_t t2 = cycles();
    if (cyc) { cyc->hb1 += t1 - t0; cyc->hb2 += t2 - t1; }
    t0 = t2;
  } else if (cand_ == Candidate::b_q15_full || cand_ == Candidate::d_q15_sparse ||
             cand_ == Candidate::d2_q15_specialized || cand_ == Candidate::d3_q15_div8) {
    auto* comb2 = static_cast<Cs16*>(hb2_buf_);
    const bool sparse = cand_ != Candidate::b_q15_full;
    const bool specialized = cand_ == Candidate::d2_q15_specialized ||
                             cand_ == Candidate::d3_q15_div8;
    const size_t n1 =
        specialized ? hb1_run(x, n, hb1_hist_, hb1_phase_, comb2 + kH2,
                              [](const Cu8* p, Cs16& y) { hb1_specialized(p, y); })
        : sparse ? hb1_run(x, n, hb1_hist_, hb1_phase_, comb2 + kH2,
                         [](const Cu8* p, Cs16& y) { hb1_sparse(p, y); })
               : hb1_run(x, n, hb1_hist_, hb1_phase_, comb2 + kH2,
                         [](const Cu8* p, Cs16& y) { hb1_full(p, y); });
    uint32_t t1 = cycles();
    if (pp_ == PpKind::f32) {
      auto* pp_in = static_cast<Cf32*>(pp_buf_) + H3;
      const auto store = [pp_in](size_t o, int32_t ai, int32_t aq) {
        pp_in[o].i = static_cast<float>(ai >> 15) * kQ15ToC;
        pp_in[o].q = static_cast<float>(aq >> 15) * kQ15ToC;
      };
      n2 = sparse ? hb2_run(comb2, n1, hb2_phase_, [&](const Cs16* p, size_t o) {
                      int32_t ai, aq; hb2_sparse(p, ai, aq); store(o, ai, aq); })
                  : hb2_run(comb2, n1, hb2_phase_, [&](const Cs16* p, size_t o) {
                      int32_t ai, aq; hb2_full(p, ai, aq); store(o, ai, aq); });
    } else {
      const bool div8 = cand_ == Candidate::d3_q15_div8;
      auto* hb2_out = div8 ? static_cast<Cs16*>(hb3_buf_) + kHb3Hist
                           : static_cast<Cs16*>(pp_buf_) + H3;
      const auto store = [hb2_out](size_t o, int32_t ai, int32_t aq) {
        hb2_out[o].i = static_cast<int16_t>(ai >> 15);
        hb2_out[o].q = static_cast<int16_t>(aq >> 15);
      };
      n2 = specialized ? hb2_run(comb2, n1, hb2_phase_, [&](const Cs16* p, size_t o) {
                            int32_t ai, aq; hb2_specialized(p, ai, aq); store(o, ai, aq); })
           : sparse ? hb2_run(comb2, n1, hb2_phase_, [&](const Cs16* p, size_t o) {
                       int32_t ai, aq; hb2_sparse(p, ai, aq); store(o, ai, aq); })
                  : hb2_run(comb2, n1, hb2_phase_, [&](const Cs16* p, size_t o) {
                      int32_t ai, aq; hb2_full(p, ai, aq); store(o, ai, aq); });
    }
    uint32_t t2 = cycles();
    if (cyc) { cyc->hb1 += t1 - t0; cyc->hb2 += t2 - t1; }
    t0 = t2;
    if (cand_ == Candidate::d3_q15_div8) {
      n2 = hb3_run(static_cast<Cs16*>(hb3_buf_), n2, hb3_phase_,
                   static_cast<Cs16*>(pp_buf_) + H3);
      const uint32_t t3 = cycles();
      if (cyc) cyc->hb3 += t3 - t2;
      t0 = t3;
    }
  } else {  // ESP-DSP
    for (size_t k = 0; k < n; ++k) {
      c_i0_[k] = static_cast<int16_t>(static_cast<int32_t>(x[k].i) - 128);
      c_q0_[k] = static_cast<int16_t>(static_cast<int32_t>(x[k].q) - 128);
    }
    uint32_t tc = cycles();
    auto* fir = static_cast<fir_s16_t*>(c_fir_);
    const bool arp4 = cand_ == Candidate::c_espdsp_arp4;
    const int32_t o1 = static_cast<int32_t>(n / 2), o2 = static_cast<int32_t>(n / 4);
    if (arp4) {
      dsps_fird_s16_arp4(&fir[0], c_i0_, c_i1_, o1);
      dsps_fird_s16_arp4(&fir[1], c_q0_, c_q1_, o1);
    } else {
      dsps_fird_s16_ansi(&fir[0], c_i0_, c_i1_, o1);
      dsps_fird_s16_ansi(&fir[1], c_q0_, c_q1_, o1);
    }
    uint32_t t1 = cycles();
    if (arp4) {
      dsps_fird_s16_arp4(&fir[2], c_i1_, c_i2_, o2);
      dsps_fird_s16_arp4(&fir[3], c_q1_, c_q2_, o2);
    } else {
      dsps_fird_s16_ansi(&fir[2], c_i1_, c_i2_, o2);
      dsps_fird_s16_ansi(&fir[3], c_q1_, c_q2_, o2);
    }
    uint32_t t2 = cycles();
    if (pp_ == PpKind::f32) {
      auto* pp_in = static_cast<Cf32*>(pp_buf_) + H3;
      for (int32_t k = 0; k < o2; ++k) {
        pp_in[k].i = static_cast<float>(c_i2_[k]) * kQ15ToC;
        pp_in[k].q = static_cast<float>(c_q2_[k]) * kQ15ToC;
      }
    } else {
      auto* pp_in = static_cast<Cs16*>(pp_buf_) + H3;
      for (int32_t k = 0; k < o2; ++k) pp_in[k] = {c_i2_[k], c_q2_[k]};
    }
    n2 = static_cast<size_t>(o2);
    uint32_t t3 = cycles();
    if (cyc) {
      cyc->other += (tc - t0) + (t3 - t2);  // de-interleave + interleave
      cyc->hb1 += t1 - tc;
      cyc->hb2 += t2 - t1;
    }
    t0 = t3;
  }

  // Polyphase L/M channel filter + resampler.
  size_t o = 0;
  uint32_t i = pp_i_;
  uint16_t p = pp_p_;
  const uint32_t end = H3 + static_cast<uint32_t>(n2);
  if (cand_ == Candidate::e_pie_pp || cand_ == Candidate::e_pie_all) {
    // PIE: 8 x int16 MACs per esp.vmulas into the 40-bit xacc; the sums are
    // exact integers < 2^31 (see header), so the result equals D + pp_q15.
    // Windows start at any sample, so unaligned 128-bit loads are enabled.
    const PieUnaligned unaligned;
    const bool seven = P8_ == 56;
    while (i < end) {
      pie_load_coef(e_coef_ + p * P8_, seven ? 7 : 6);
      const int16_t* xi = e_i_ + (i - H3);
      const int16_t* xq = e_q_ + (i - H3);
      const int32_t ri = seven ? pie_dot7(xi, 0, 0) : pie_dot6(xi, 0, 0);
      const int32_t rq = seven ? pie_dot7(xq, 0, 0) : pie_dot6(xq, 0, 0);
      out[o].i = static_cast<float>(ri) * kPpQ15ToC;
      out[o].q = static_cast<float>(rq) * kPpQ15ToC;
      ++o;
      i += pp_adv_[2 * p];
      p = pp_adv_[2 * p + 1];
    }
    memmove(e_i_, e_i_ + n2, sizeof(int16_t) * H3);
    memmove(e_q_, e_q_ + n2, sizeof(int16_t) * H3);
  } else if (pp_ == PpKind::f32) {
    auto* comb = static_cast<Cf32*>(pp_buf_);
    while (i < end) {
      const Cf32* xw = comb + (i - H3);
      const float* h = pp_f_ + p * P_;
      float ai0 = 0.0f, aq0 = 0.0f, ai1 = 0.0f, aq1 = 0.0f;
      int j = 0;
      for (; j + 1 < P_; j += 2) {
        ai0 += h[j] * xw[j].i;
        aq0 += h[j] * xw[j].q;
        ai1 += h[j + 1] * xw[j + 1].i;
        aq1 += h[j + 1] * xw[j + 1].q;
      }
      if (j < P_) {
        ai0 += h[j] * xw[j].i;
        aq0 += h[j] * xw[j].q;
      }
      out[o].i = ai0 + ai1;
      out[o].q = aq0 + aq1;
      ++o;
      i += pp_adv_[2 * p];
      p = pp_adv_[2 * p + 1];
    }
    memmove(comb, comb + n2, sizeof(Cf32) * H3);
  } else {
    auto* comb = static_cast<Cs16*>(pp_buf_);
    if (check_) {  // lab only: verify the documented stage bound
      int32_t peak = peak_q15_;
      for (size_t k = 0; k < n2; ++k) {
        const int32_t a = comb[H3 + k].i < 0 ? -comb[H3 + k].i : comb[H3 + k].i;
        const int32_t b = comb[H3 + k].q < 0 ? -comb[H3 + k].q : comb[H3 + k].q;
        peak = std::max(peak, std::max(a, b));
      }
      peak_q15_ = peak;
    }
    if (cand_ == Candidate::d2_q15_specialized) {
      switch (device_rate_) {
        case 2400000u: o = d2_poly<co::kPp240Q15, 2, 5, 44>(comb, i, p, end, out); break;
        case 2560000u: o = d2_poly<co::kPp256Q15, 3, 8, 47>(comb, i, p, end, out); break;
        case 2880000u: o = d2_poly<co::kPp288Q15, 1, 3, 54>(comb, i, p, end, out); break;
        case 3200000u: o = d2_poly<co::kPp320Q15, 3, 10, 59>(comb, i, p, end, out); break;
      }
    } else if (cand_ == Candidate::d3_q15_div8) {
      switch (device_rate_) {
        case 2400000u: o = d3_poly<co::kPpDiv8240Q15, 4, 5, 22>(comb, i, p, end, out); break;
        case 2560000u: o = d3_poly<co::kPpDiv8256Q15, 3, 4, 24>(comb, i, p, end, out); break;
        case 2880000u: o = d3_poly<co::kPpDiv8288Q15, 2, 3, 27>(comb, i, p, end, out); break;
        case 3200000u: o = d3_poly<co::kPpDiv8320Q15, 3, 5, 30>(comb, i, p, end, out); break;
      }
    } else while (i < end) {
      const Cs16* xw = comb + (i - H3);
      const int16_t* h = pp_q_ + p * P_;
      int32_t ai0 = 0, aq0 = 0, ai1 = 0, aq1 = 0;
      int j = 0;
      for (; j + 1 < P_; j += 2) {
        ai0 += h[j] * static_cast<int32_t>(xw[j].i);
        aq0 += h[j] * static_cast<int32_t>(xw[j].q);
        ai1 += h[j + 1] * static_cast<int32_t>(xw[j + 1].i);
        aq1 += h[j + 1] * static_cast<int32_t>(xw[j + 1].q);
      }
      if (j < P_) {
        ai0 += h[j] * static_cast<int32_t>(xw[j].i);
        aq0 += h[j] * static_cast<int32_t>(xw[j].q);
      }
      out[o].i = static_cast<float>(ai0 + ai1) * kPpQ15ToC;
      out[o].q = static_cast<float>(aq0 + aq1) * kPpQ15ToC;
      ++o;
      i += pp_adv_[2 * p];
      p = pp_adv_[2 * p + 1];
    }
    memmove(comb, comb + n2, sizeof(Cs16) * H3);
  }
  pp_i_ = i - static_cast<uint32_t>(n2);
  pp_p_ = p;
  if (cyc) cyc->pp += cycles() - t0;
  return o;
}

}  // namespace orcsdr::dsp::mr
