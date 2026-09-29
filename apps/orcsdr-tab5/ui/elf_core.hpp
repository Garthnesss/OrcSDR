#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace orcsdr::elfcore {

enum class ChordType : uint8_t {
  major, minor, sus4, diminished, sus2, augmented,
  minor_add9, seven_sus4, power, minor7_flat5, major_flat5, sus2_flat5
};

enum Extension : uint8_t {
  ext_none = 0,
  ext_6 = 1u << 0,
  ext_m7 = 1u << 1,
  ext_M7 = 1u << 2,
  ext_9 = 1u << 3,
};
using ExtensionMask = uint8_t;

enum class VoicingStyle : uint8_t { closed, open, drop2, drop3, spread, drop24, low_root };

constexpr size_t kMaxChordNotes = 12;

struct NoteSet {
  std::array<int16_t, kMaxChordNotes> notes{};
  uint8_t count = 0;
};

struct EngineState {
  int root_c = 48;
  int octave_offset = 0;
  int voicing_walk = 0;
  VoicingStyle style = VoicingStyle::closed;
  ChordType type = ChordType::major;
  ExtensionMask extensions = ext_none;
};

const char* chord_type_name(ChordType type);
const char* voicing_style_name(VoicingStyle style);
NoteSet build_chord(int pitch_class, ChordType type, ExtensionMask extensions,
                    int root_c = 48, int octave_offset = 0);
NoteSet apply_inversion(const NoteSet& pitches, int inversion_index);
NoteSet apply_voicing_style(const NoteSet& pitches, VoicingStyle style);
NoteSet walk_voicing(const NoteSet& pitches, int steps, VoicingStyle style);
NoteSet render_chord(const EngineState& state, int pitch_class);
bool valid_midi_chord(const NoteSet& notes);
bool self_check();

}  // namespace orcsdr::elfcore
