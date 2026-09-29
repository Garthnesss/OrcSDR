#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace orcsdr::elfsynth {

enum class Waveform : uint8_t { saw, square, triangle, sine };

struct Patch {
  Waveform waveform = Waveform::saw;
  float attack_ms = 18.0f;
  float decay_ms = 320.0f;
  float sustain = 0.68f;
  float release_ms = 900.0f;
  float cutoff_hz = 4200.0f;
  float master_gain = 0.16f;
};

class Synth {
 public:
  static constexpr uint32_t kSampleRate = 48000;
  static constexpr size_t kVoices = 12;

  Synth();
  void reset();
  void set_patch(const Patch& patch);
  const Patch& patch() const { return patch_; }
  void note_on(uint8_t note, uint8_t velocity = 110);
  void note_off(uint8_t note);
  void all_notes_off();
  void render(int16_t* mono, size_t frames);
  size_t active_voice_count() const;

 private:
  enum class EnvStage : uint8_t { off, attack, decay, sustain, release };
  struct Voice {
    bool active = false;
    uint8_t note = 0;
    uint8_t velocity = 0;
    float phase = 0.0f;
    float phase_inc = 0.0f;
    float env = 0.0f;
    float release_step = 0.0f;
    EnvStage stage = EnvStage::off;
    uint32_t age = 0;
  };

  std::array<Voice,kVoices> voices_{};
  Patch patch_{};
  uint32_t age_counter_ = 1;
  float lp_state_ = 0.0f;
  float lp_alpha_ = 0.35f;

  Voice* allocate_voice(uint8_t note);
  float oscillator(Voice& voice) const;
  float advance_envelope(Voice& voice);
  void update_filter();
};

Patch orc_pad();
bool self_check();

}  // namespace orcsdr::elfsynth
