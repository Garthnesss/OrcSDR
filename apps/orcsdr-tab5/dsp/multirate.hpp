#pragma once

#include <cstddef>
#include <cstdint>

// Stage-2 multirate frontend candidates (benchmark lab only; not connected to
// any live demodulator). CU8 -> halfband /2 -> halfband /2 -> rational L/M
// polyphase channel filter -> complex float at 240 kS/s.
//
// Numeric contract (Q formats), see docs/dsp/DSP_ARCHITECTURE_AUDIT.md §15:
//  - CU8 centering: c = u - 128, an integer in [-128, 127]. The float oracle
//    uses the same c, so every candidate sees identical input.
//  - Stage buffers (Cs16) hold c * 128: CU8 full scale 128 -> 16384, which
//    leaves 6 dB of headroom and 7 fractional bits below the input LSB.
//  - Coefficients: Q15 (int16, value * 32768, rounded).
//  - Accumulators: int32. Worst cases (proved in the design script):
//      HB1  128 * 32768 * sum|h1|            =   4.8e6
//      HB2  18654 * 32768 * sum|h2|          =   7.9e8
//      PP   24170 * max phase sum|h_q15|     =   1.4e9   (< 2^31 = 2.1e9)
//  - Rounding: round-half-up (add 2^(s-1) before >> s) at every narrowing.
//  - Saturation: cannot occur for any CU8 input (worst |HB2 out| = 24170 <
//    32767), so the hot loops do not saturate; the lab verifies the bound.
//  - Output: complex float in c units (same scale the demodulators use today).
namespace orcsdr::dsp::mr {

struct Cu8 { uint8_t i, q; };
struct Cs16 { int16_t i, q; };
struct Cf32 { float i, q; };

enum class Candidate : uint8_t {
  a_float,        // A: scalar float reference (all taps)
  b_q15_full,     // B: scalar Q15, all taps (zeros included), outputs only
  c_espdsp_ansi,  // C: ESP-DSP dsps_fird_s16_ansi (I and Q de-interleaved)
  c_espdsp_arp4,  // C: ESP-DSP dsps_fird_s16_arp4 (P4 SIMD + HWLOOP)
  d_q15_sparse,   // D: custom Q15, zero taps skipped, symmetric pairs folded
  e_pie_pp,       // E1: D halfbands + P4 PIE SIMD Q15 polyphase (unrolled, no
                  //     esp.lp.setup / HWLOOP); bit-identical to D + pp_q15
  e_pie_all,      // E2: PIE for both halfbands and the polyphase (dot products
                  //     into xacc with D's rounding bias); bit-identical to D + pp_q15
  count
};
enum class PpKind : uint8_t { f32, q15 };
// Initialization is first-class: process() does nothing unless ready, so an
// allocation or init failure can never turn into a spin (watchdog reset).
enum class InitState : uint8_t { uninitialized, ready, failed };

const char* candidate_name(Candidate c);
const char* pp_name(PpKind p);

struct StageCycles {
  uint32_t hb1 = 0, hb2 = 0, pp = 0, other = 0;
  void clear() { hb1 = hb2 = pp = other = 0; }
  uint32_t total() const { return hb1 + hb2 + pp + other; }
};

struct MemUse {
  size_t internal = 0, psram = 0;
};

class Frontend {
 public:
  Frontend() = default;
  ~Frontend();
  Frontend(const Frontend&) = delete;
  Frontend& operator=(const Frontend&) = delete;

  // device_rate must be one of 2.40/2.56/2.88/3.20 MS/s. chunk_in: input
  // samples per internal pass (multiple of 4); buffers are sized from it.
  bool init(uint32_t device_rate, Candidate cand, PpKind pp, size_t chunk_in,
            bool internal_buffers);
  void release();
  void reset();  // clear all filter state (discontinuity)

  // Consumes n complex CU8 samples. ESP-DSP buffers up to three samples so
  // its two /2 filters see complete groups of four across call boundaries.
  size_t process(const uint8_t* cu8, size_t n, Cf32* out, size_t out_cap,
                 StageCycles* cycles = nullptr);

  size_t max_out(size_t n) const;  // upper bound of outputs for n inputs
  InitState state() const { return state_; }
  bool ready() const { return state_ == InitState::ready; }
  // ESP-DSP candidates: the four filter structs (hb1 I/Q, hb2 I/Q) as
  // fir_s16_t*, for post-init inspection. nullptr otherwise.
  const void* espdsp_fir(int k) const;
  uint32_t device_rate() const { return device_rate_; }
  uint16_t L() const { return L_; }
  uint16_t M() const { return M_; }
  uint16_t pp_taps() const { return taps_; }
  uint16_t pp_phase_taps() const { return P_; }
  MemUse mem() const { return mem_; }
  // Lab: track max |stage value| entering a Q15 polyphase (outside timing).
  void set_check(bool on) { check_ = on; }
  int32_t peak_q15() const { return peak_q15_; }
  bool arp4_active() const { return arp4_active_; }
  Candidate candidate() const { return cand_; }
  PpKind pp_kind() const { return pp_; }

 private:
  void* alloc(size_t bytes);
  size_t chunk(const uint8_t* cu8, size_t n, Cf32* out, StageCycles* cycles);
  bool init_impl(uint32_t device_rate, Candidate cand, PpKind pp, size_t chunk_in,
                 bool internal_buffers);

  InitState state_ = InitState::uninitialized;

  uint32_t device_rate_ = 0;
  Candidate cand_ = Candidate::a_float;
  PpKind pp_ = PpKind::f32;
  size_t chunk_in_ = 0;
  bool internal_ = false;
  MemUse mem_{};
  void* blocks_[16]{};
  int nblocks_ = 0;

  // HB1 stitch history (CU8) and decimation phase.
  Cu8 hb1_hist_[8]{};
  Cf32 hb1_hist_f_[8]{};
  uint8_t hb1_phase_ = 0;
  uint8_t hb2_phase_ = 0;

  // HB2 input buffer: history (kHb2Taps - 1) then up to chunk_in/2 samples.
  void* hb2_buf_ = nullptr;
  // PP input buffer: history (P - 1) then up to chunk_in/4 samples.
  void* pp_buf_ = nullptr;

  // ESP-DSP (candidate C): de-interleaved stage arrays and filter structs.
  int16_t* c_i0_ = nullptr;
  int16_t* c_q0_ = nullptr;
  int16_t* c_i1_ = nullptr;
  int16_t* c_q1_ = nullptr;
  int16_t* c_i2_ = nullptr;
  int16_t* c_q2_ = nullptr;
  void* c_fir_ = nullptr;  // 4 x fir_s16_t (hb1 I/Q, hb2 I/Q)
  int16_t* c_hb1_coeffs_ = nullptr;
  int16_t* c_hb2_coeffs_ = nullptr;
  bool arp4_active_ = false;
  Cu8 c_pending_[4]{};
  uint8_t c_pending_n_ = 0;

  // Polyphase.
  uint16_t L_ = 1, M_ = 1, P_ = 0, taps_ = 0;
  float* pp_f_ = nullptr;      // [L][P] reversed per phase, float
  int16_t* pp_q_ = nullptr;    // [L][P] reversed per phase, Q15
  uint16_t* pp_adv_ = nullptr; // input advance after an output at phase p
  uint32_t pp_i_ = 0;          // newest input (combined coords) for next output
  uint16_t pp_p_ = 0;          // current phase

  int32_t peak_q15_ = 0;
  bool check_ = false;

  // Candidate E: de-interleaved PP inputs (history P-1, then data, then 8
  // spare so a padded vector never reads past the buffer) and zero-padded
  // 16-byte-aligned per-phase coefficient vectors of P8 = ceil(P/8)*8 taps.
  int16_t* e_i_ = nullptr;
  int16_t* e_q_ = nullptr;
  int16_t* e_coef_ = nullptr;
  uint16_t P8_ = 0;
  // Candidate E2: de-interleaved centered CU8 (history 6) and HB1 output
  // (history 14), each + 8 spare, and the padded halfband coefficient vectors.
  int16_t* e1i_ = nullptr;
  int16_t* e1q_ = nullptr;
  int16_t* e2i_ = nullptr;
  int16_t* e2q_ = nullptr;
  int16_t* e_hb1c_ = nullptr;  // 8 lanes: [h1 reversed, 0]
  int16_t* e_hb2c_ = nullptr;  // 16 lanes: [h2 reversed, 0]
};

}  // namespace orcsdr::dsp::mr
