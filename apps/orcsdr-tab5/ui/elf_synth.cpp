#include "elf_synth.hpp"
#include <algorithm>
#include <cmath>

namespace orcsdr::elfsynth {
namespace {
constexpr float kTwoPi = 6.2831853071795864769f;
float midi_hz(uint8_t note) {
  return 440.0f * exp2f((static_cast<int>(note)-69)/12.0f);
}
float clamp1(float v) { return std::max(-1.0f,std::min(1.0f,v)); }
}

Synth::Synth() { set_patch(orc_pad()); reset(); }

void Synth::reset() {
  for(auto& v:voices_) v={};
  age_counter_=1;
  lp_state_=0.0f;
}

void Synth::set_patch(const Patch& patch) {
  patch_=patch;
  patch_.attack_ms=std::max(1.0f,patch_.attack_ms);
  patch_.decay_ms=std::max(1.0f,patch_.decay_ms);
  patch_.release_ms=std::max(1.0f,patch_.release_ms);
  patch_.sustain=std::max(0.0f,std::min(1.0f,patch_.sustain));
  patch_.master_gain=std::max(0.0f,std::min(0.5f,patch_.master_gain));
  patch_.cutoff_hz=std::max(80.0f,std::min(18000.0f,patch_.cutoff_hz));
  update_filter();
}

void Synth::update_filter() {
  const float x=expf(-kTwoPi*patch_.cutoff_hz/static_cast<float>(kSampleRate));
  lp_alpha_=1.0f-x;
}

Synth::Voice* Synth::allocate_voice(uint8_t note) {
  for(auto& v:voices_) if(v.active && v.note==note) return &v;
  for(auto& v:voices_) if(!v.active) return &v;
  Voice* oldest=&voices_[0];
  for(auto& v:voices_) if(v.age<oldest->age) oldest=&v;
  return oldest;
}

void Synth::note_on(uint8_t note, uint8_t velocity) {
  if(note>127 || velocity==0) { note_off(note); return; }
  auto* v=allocate_voice(note);
  *v={};
  v->active=true;
  v->note=note;
  v->velocity=velocity;
  v->phase_inc=midi_hz(note)/static_cast<float>(kSampleRate);
  v->stage=EnvStage::attack;
  v->age=age_counter_++;
}

void Synth::note_off(uint8_t note) {
  for(auto& v:voices_) {
    if(!v.active || v.note!=note) continue;
    v.stage=EnvStage::release;
    const float samples=patch_.release_ms*0.001f*kSampleRate;
    v.release_step=v.env/std::max(1.0f,samples);
  }
}

void Synth::all_notes_off() {
  for(auto& v:voices_) {
    if(!v.active) continue;
    v.stage=EnvStage::release;
    v.release_step=std::max(v.env,0.001f)/(0.020f*kSampleRate);
  }
}

float Synth::oscillator(Voice& v) const {
  const float p=v.phase;
  float out=0.0f;
  switch(patch_.waveform) {
    case Waveform::saw: out=2.0f*p-1.0f; break;
    case Waveform::square: out=p<0.5f?1.0f:-1.0f; break;
    case Waveform::triangle: out=1.0f-4.0f*fabsf(p-0.5f); break;
    case Waveform::sine: out=sinf(kTwoPi*p); break;
  }
  v.phase+=v.phase_inc;
  if(v.phase>=1.0f) v.phase-=floorf(v.phase);
  return out;
}

float Synth::advance_envelope(Voice& v) {
  if(!v.active) return 0.0f;
  switch(v.stage) {
    case EnvStage::attack: {
      v.env += 1.0f/(patch_.attack_ms*0.001f*kSampleRate);
      if(v.env>=1.0f) { v.env=1.0f; v.stage=EnvStage::decay; }
      break;
    }
    case EnvStage::decay: {
      v.env -= (1.0f-patch_.sustain)/(patch_.decay_ms*0.001f*kSampleRate);
      if(v.env<=patch_.sustain) { v.env=patch_.sustain; v.stage=EnvStage::sustain; }
      break;
    }
    case EnvStage::sustain: v.env=patch_.sustain; break;
    case EnvStage::release:
      v.env-=std::max(v.release_step,1.0f/(patch_.release_ms*0.001f*kSampleRate));
      if(v.env<=0.0f) { v={}; return 0.0f; }
      break;
    case EnvStage::off: return 0.0f;
  }
  return v.env;
}

void Synth::render(int16_t* mono, size_t frames) {
  if(!mono) return;
  for(size_t i=0;i<frames;++i) {
    float mix=0.0f;
    size_t active=0;
    for(auto& v:voices_) {
      if(!v.active) continue;
      const float env=advance_envelope(v);
      if(!v.active) continue;
      const float vel=static_cast<float>(v.velocity)/127.0f;
      mix+=oscillator(v)*env*vel;
      ++active;
    }
    if(active) mix/=sqrtf(static_cast<float>(active));
    mix*=patch_.master_gain;
    lp_state_ += lp_alpha_*(mix-lp_state_);
    mono[i]=static_cast<int16_t>(clamp1(lp_state_)*32767.0f);
  }
}

size_t Synth::active_voice_count() const {
  size_t n=0; for(const auto& v:voices_) if(v.active) ++n; return n;
}

Patch orc_pad() {
  Patch p{};
  p.waveform=Waveform::saw;
  p.attack_ms=22.0f;
  p.decay_ms=360.0f;
  p.sustain=0.72f;
  p.release_ms=1100.0f;
  p.cutoff_hz=3200.0f;
  p.master_gain=0.15f;
  return p;
}

bool self_check() {
  Synth s;
  int16_t block[128]{};
  s.note_on(60,100);
  s.render(block,128);
  bool nonzero=false; for(auto x:block) if(x){ nonzero=true; break; }
  s.note_off(60);
  for(int i=0;i<500;++i) s.render(block,128);
  return nonzero && s.active_voice_count()==0;
}

}  // namespace orcsdr::elfsynth
