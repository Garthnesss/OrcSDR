#include "elf_core.hpp"
#include <algorithm>
#include <array>

namespace orcsdr::elfcore {
namespace {

struct IntervalSet { const int8_t* data; uint8_t count; };

constexpr int8_t kMajor[] = {0,4,7};
constexpr int8_t kMinor[] = {0,3,7};
constexpr int8_t kSus4[] = {0,5,7};
constexpr int8_t kDim[] = {0,3,6};
constexpr int8_t kSus2[] = {0,2,7};
constexpr int8_t kAug[] = {0,4,8};
constexpr int8_t kMadd9[] = {0,3,7,14};
constexpr int8_t k7sus4[] = {0,5,7,10};
constexpr int8_t kPower[] = {0,7,12};
constexpr int8_t kM7b5[] = {0,3,6,10};
constexpr int8_t kMajb5[] = {0,4,6};
constexpr int8_t kSus2b5[] = {0,2,6};

template<size_t N> constexpr IntervalSet iset(const int8_t (&v)[N]) {
  return {v, static_cast<uint8_t>(N)};
}

IntervalSet intervals(ChordType type) {
  switch(type) {
    case ChordType::major: return iset(kMajor);
    case ChordType::minor: return iset(kMinor);
    case ChordType::sus4: return iset(kSus4);
    case ChordType::diminished: return iset(kDim);
    case ChordType::sus2: return iset(kSus2);
    case ChordType::augmented: return iset(kAug);
    case ChordType::minor_add9: return iset(kMadd9);
    case ChordType::seven_sus4: return iset(k7sus4);
    case ChordType::power: return iset(kPower);
    case ChordType::minor7_flat5: return iset(kM7b5);
    case ChordType::major_flat5: return iset(kMajb5);
    case ChordType::sus2_flat5: return iset(kSus2b5);
  }
  return iset(kMajor);
}

int pc(int v) { int r=v%12; return r<0?r+12:r; }

bool append(NoteSet& out, int value) {
  if (out.count >= kMaxChordNotes) return false;
  out.notes[out.count++] = static_cast<int16_t>(value);
  return true;
}

void sort_unique(NoteSet& out) {
  std::sort(out.notes.begin(), out.notes.begin()+out.count);
  uint8_t write=0;
  for(uint8_t i=0;i<out.count;++i) {
    if(write==0 || out.notes[i]!=out.notes[write-1]) out.notes[write++]=out.notes[i];
  }
  out.count=write;
}

bool has_pc(const NoteSet& notes, int pitch_class, int root) {
  for(uint8_t i=0;i<notes.count;++i) if(pc(notes.notes[i]-root)==pitch_class) return true;
  return false;
}

int ext_interval(uint8_t bit) {
  switch(bit) {
    case ext_6: return 9;
    case ext_m7: return 10;
    case ext_M7: return 11;
    case ext_9: return 14;
    default: return -1;
  }
}

}  // namespace

const char* chord_type_name(ChordType type) {
  switch(type) {
    case ChordType::major: return "MAJ";
    case ChordType::minor: return "MIN";
    case ChordType::sus4: return "SUS";
    case ChordType::diminished: return "DIM";
    case ChordType::sus2: return "SUS2";
    case ChordType::augmented: return "AUG";
    case ChordType::minor_add9: return "mADD9";
    case ChordType::seven_sus4: return "7SUS4";
    case ChordType::power: return "POWER";
    case ChordType::minor7_flat5: return "m7b5";
    case ChordType::major_flat5: return "MAJb5";
    case ChordType::sus2_flat5: return "SUS2b5";
  }
  return "MAJ";
}

const char* voicing_style_name(VoicingStyle style) {
  switch(style) {
    case VoicingStyle::closed: return "Closed";
    case VoicingStyle::open: return "Open";
    case VoicingStyle::drop2: return "Drop 2";
    case VoicingStyle::drop3: return "Drop 3";
    case VoicingStyle::spread: return "Spread";
    case VoicingStyle::drop24: return "Drop 2+4";
    case VoicingStyle::low_root: return "Low Root";
  }
  return "Closed";
}

NoteSet build_chord(int pitch_class, ChordType type, ExtensionMask exts,
                    int root_c, int octave_offset) {
  NoteSet out{};
  const int root = root_c + pitch_class + 12*octave_offset;
  const auto iv=intervals(type);
  for(uint8_t i=0;i<iv.count;++i) append(out, root+iv.data[i]);
  constexpr uint8_t bits[] = {ext_6, ext_m7, ext_M7, ext_9};
  for(uint8_t bit:bits) {
    if(!(exts&bit)) continue;
    const int e=ext_interval(bit);
    if(e<0 || has_pc(out, pc(e), root)) continue;
    append(out, root+e);
  }
  sort_unique(out);
  return out;
}

NoteSet apply_inversion(const NoteSet& pitches, int inversion_index) {
  NoteSet out=pitches;
  if(!out.count) return out;
  sort_unique(out);
  int idx=inversion_index;
  if(idx<0) idx=(idx%out.count+out.count)%out.count;
  for(int step=0;step<idx;++step) {
    const int low=out.notes[0]+12;
    for(uint8_t i=1;i<out.count;++i) out.notes[i-1]=out.notes[i];
    out.notes[out.count-1]=low;
    sort_unique(out);
  }
  return out;
}

NoteSet apply_voicing_style(const NoteSet& pitches, VoicingStyle style) {
  NoteSet out=pitches;
  sort_unique(out);
  if(style==VoicingStyle::closed) return out;
  if(style==VoicingStyle::open && out.count>=2) {
    out.notes[1]+=12;
  } else if(style==VoicingStyle::drop2 && out.count>=4) {
    out.notes[out.count-2]-=12;
  } else if(style==VoicingStyle::drop3 && out.count>=4) {
    out.notes[out.count-3]-=12;
  } else if(style==VoicingStyle::spread && out.count>=3) {
    for(uint8_t i=1;i<out.count;i+=2) out.notes[i]+=12;
  } else if(style==VoicingStyle::drop24 && out.count>=4) {
    out.notes[out.count-2]-=12;
    out.notes[out.count-4]-=12;
  } else if(style==VoicingStyle::low_root && out.count>=3) {
    out.notes[0]-=12;
  }
  sort_unique(out);
  return out;
}

bool valid_midi_chord(const NoteSet& notes) {
  if(!notes.count) return false;
  for(uint8_t i=0;i<notes.count;++i) if(notes.notes[i]<0 || notes.notes[i]>127) return false;
  return true;
}

NoteSet walk_voicing(const NoteSet& pitches, int steps, VoicingStyle style) {
  NoteSet work=pitches;
  sort_unique(work);
  NoteSet best=apply_voicing_style(work,style);
  const int direction=steps>=0?1:-1;
  for(int s=0;s<(steps>=0?steps:-steps);++s) {
    if(!work.count) break;
    if(direction>0) {
      int moved=work.notes[0]+12;
      const int top=work.notes[work.count-1];
      while(moved<=top) moved+=12;
      for(uint8_t i=1;i<work.count;++i) work.notes[i-1]=work.notes[i];
      work.notes[work.count-1]=moved;
    } else {
      int moved=work.notes[work.count-1]-12;
      const int bottom=work.notes[0];
      while(moved>=bottom) moved-=12;
      for(int i=work.count-1;i>0;--i) work.notes[i]=work.notes[i-1];
      work.notes[0]=moved;
    }
    sort_unique(work);
    const auto candidate=apply_voicing_style(work,style);
    if(!valid_midi_chord(candidate)) break;
    best=candidate;
  }
  return best;
}

NoteSet render_chord(const EngineState& state, int pitch_class) {
  return walk_voicing(build_chord(pitch_class,state.type,state.extensions,
                                  state.root_c,state.octave_offset),
                      state.voicing_walk,state.style);
}

bool self_check() {
  const auto c=build_chord(0,ChordType::major,ext_M7,48,0);
  if(c.count!=4 || c.notes[0]!=48 || c.notes[1]!=52 || c.notes[2]!=55 || c.notes[3]!=59) return false;
  const auto inv=apply_inversion(build_chord(0,ChordType::major,ext_none,48,0),1);
  if(inv.count!=3 || inv.notes[0]!=52 || inv.notes[1]!=55 || inv.notes[2]!=60) return false;
  const auto sus9=build_chord(0,ChordType::sus2,ext_9,48,0);
  if(sus9.count!=3) return false; // the 9 duplicates sus2's pitch class
  return valid_midi_chord(c);
}

}  // namespace orcsdr::elfcore
