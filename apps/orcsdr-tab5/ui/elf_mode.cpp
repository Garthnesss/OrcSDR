#include "elf_mode.hpp"

#include <M5GFX.h>
#include <M5Unified.h>

#include <array>
#include <cstdio>

#include "elf_core.hpp"
#include "elf_synth.hpp"

namespace orcsdr::elf_mode {
namespace {

constexpr int32_t kScreenW = 1280;
constexpr int32_t kScreenH = 720;

constexpr uint16_t kBg = TFT_BLACK;
constexpr uint16_t kPanel = 0x0841;
constexpr uint16_t kPanel2 = 0x10A2;
constexpr uint16_t kCyan = 0x05FF;
constexpr uint16_t kGreen = 0x6FE0;
constexpr uint16_t kGold = 0xFEA0;
constexpr uint16_t kMuted = 0x8C71;

enum class Page : uint8_t { splash, instrument };

bool g_active = false;
bool g_holding = false;
bool g_unlock_latched = false;
bool g_touch_was_pressed = false;
bool g_exit_requested = false;
uint32_t g_hold_started_ms = 0;
screens::Id g_return_to = screens::Id::radio;
Page g_page = Page::splash;

elfcore::EngineState g_engine{};
elfsynth::Synth g_synth{};
elfcore::NoteSet g_active_notes{};
int g_active_root_index = -1;

constexpr int kRootPcs[7] = {0,2,4,5,7,9,11};
constexpr const char* kRootNames[7] = {"C","D","E","F","G","A","B"};

bool hit(int32_t x, int32_t y, int32_t bx, int32_t by, int32_t bw, int32_t bh) {
  return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

void center_text(const char* value, int32_t x, int32_t y, uint16_t color, int size,
                 uint16_t bg = kBg) {
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(color, bg);
  M5.Display.setTextSize(size);
  M5.Display.drawString(value, x, y);
}

void button(int x, int y, int w, int h, const char* label, bool selected=false) {
  const uint16_t fill = selected ? 0x19C3 : kPanel;
  const uint16_t edge = selected ? kGreen : kMuted;
  M5.Display.fillRoundRect(x,y,w,h,12,fill);
  M5.Display.drawRoundRect(x,y,w,h,12,edge);
  center_text(label,x+w/2,y+h/2,selected?kGreen:TFT_WHITE,2,fill);
}

void draw_splash() {
  M5.Display.clearScrollRect();
  M5.Display.fillScreen(kBg);

  // Phase 1 keeps the splash rendered through M5GFX only. The approved
  // illustrated Elf/lute/waterfall artwork can replace this background asset
  // without changing the lifecycle or touch geometry.
  for (int x=0; x<kScreenW; x+=12) {
    const int h = 60 + ((x * 37) % 220);
    M5.Display.drawFastVLine(x, 520-h, h, (x%48==0)?kGreen:kCyan);
  }

  center_text("440.000 MHz",640,54,kCyan,4);
  center_text("SIGNAL LOCK",640,102,kGreen,2);
  center_text("UNKNOWN PROTOCOL",640,142,kMuted,2);
  center_text("ELFCHORDS",640,292,kGold,6);
  center_text("SECRET INSTRUMENT",640,362,TFT_WHITE,3);
  center_text("ELF + LUTE + WATERFALL SPLASH",640,416,kMuted,2);

  button(410,520,460,88,"ENTER ELF MODE",true);
  button(1010,620,230,62,"RETURN TO ORCSDR",false);
}

void draw_note_line() {
  M5.Display.fillRect(150,545,980,44,kBg);
  char buf[160]{};
  int n = snprintf(buf,sizeof(buf),"MIDI:");
  for(uint8_t i=0;i<g_active_notes.count && n < static_cast<int>(sizeof(buf)-8);++i)
    n += snprintf(buf+n,sizeof(buf)-n," %d",static_cast<int>(g_active_notes.notes[i]));
  center_text(buf,640,566,g_active_notes.count?kGreen:kMuted,2);
}

void draw_instrument() {
  M5.Display.clearScrollRect();
  M5.Display.fillScreen(kBg);

  center_text("ELFCHORDS",180,45,kGold,3);
  center_text("ORC PAD",640,45,kGreen,2);
  char voices[48]{};
  snprintf(voices,sizeof(voices),"VOICES %u",static_cast<unsigned>(g_synth.active_voice_count()));
  center_text(voices,1040,45,kCyan,2);

  center_text("CHORD TYPE",120,110,kMuted,2);
  constexpr int typeY=142, typeW=190, typeH=74;
  button(180,typeY,typeW,typeH,"MAJ",g_engine.type==elfcore::ChordType::major);
  button(390,typeY,typeW,typeH,"MIN",g_engine.type==elfcore::ChordType::minor);
  button(600,typeY,typeW,typeH,"SUS",g_engine.type==elfcore::ChordType::sus4);
  button(810,typeY,typeW,typeH,"DIM",g_engine.type==elfcore::ChordType::diminished);

  center_text("EXTENSIONS",130,262,kMuted,2);
  button(180,292,190,70,"6",(g_engine.extensions&elfcore::ext_6)!=0);
  button(390,292,190,70,"m7",(g_engine.extensions&elfcore::ext_m7)!=0);
  button(600,292,190,70,"M7",(g_engine.extensions&elfcore::ext_M7)!=0);
  button(810,292,190,70,"9",(g_engine.extensions&elfcore::ext_9)!=0);

  center_text("ROOTS",90,415,kMuted,2);
  constexpr int rootY=445, rootW=130, rootH=82, gap=18, startX=145;
  for(int i=0;i<7;++i)
    button(startX+i*(rootW+gap),rootY,rootW,rootH,kRootNames[i],g_active_root_index==i);

  draw_note_line();
  button(1010,620,230,62,"RETURN TO ORCSDR",false);
}

void draw_current() {
  if(g_page==Page::splash) draw_splash();
  else draw_instrument();

  if (M5.Display.width()!=kScreenW || M5.Display.height()!=kScreenH) {
    M5.Display.setTextDatum(top_left);
    M5.Display.setTextColor(TFT_YELLOW,kBg);
    M5.Display.setTextSize(1);
    M5.Display.drawString("ELF UI TARGET: TAB5 1280x720",12,694);
  }
}

void release_active_chord() {
  for(uint8_t i=0;i<g_active_notes.count;++i)
    g_synth.note_off(static_cast<uint8_t>(g_active_notes.notes[i]));
  g_active_notes={};
  g_active_root_index=-1;
}

void play_root(int index) {
  if(index<0 || index>=7) return;
  release_active_chord();
  g_active_notes=elfcore::render_chord(g_engine,kRootPcs[index]);
  if(!elfcore::valid_midi_chord(g_active_notes)) {
    g_active_notes={};
    return;
  }
  for(uint8_t i=0;i<g_active_notes.count;++i)
    g_synth.note_on(static_cast<uint8_t>(g_active_notes.notes[i]),112);
  g_active_root_index=index;
}

void enter(screens::Id return_to, uint32_t now_ms) {
  if(g_active) return;
  g_return_to=(return_to==screens::Id::none || return_to==screens::Id::elf)
                  ? screens::Id::radio : return_to;
  g_exit_requested=false;
  g_touch_was_pressed=true;
  g_page=Page::splash;
  g_engine={};
  g_synth.reset();
  g_synth.set_patch(elfsynth::orc_pad());
  g_active_notes={};
  g_active_root_index=-1;

  screens::begin_transition(screens::Id::elf,now_ms);
  g_active=true;
  draw_current();
  screens::finish_transition();
}

void select_type(int32_t x,int32_t y) {
  if(hit(x,y,180,142,190,74)) g_engine.type=elfcore::ChordType::major;
  else if(hit(x,y,390,142,190,74)) g_engine.type=elfcore::ChordType::minor;
  else if(hit(x,y,600,142,190,74)) g_engine.type=elfcore::ChordType::sus4;
  else if(hit(x,y,810,142,190,74)) g_engine.type=elfcore::ChordType::diminished;
  else return;
  release_active_chord();
  draw_instrument();
}

void toggle_ext(int32_t x,int32_t y) {
  uint8_t bit=0;
  if(hit(x,y,180,292,190,70)) bit=elfcore::ext_6;
  else if(hit(x,y,390,292,190,70)) bit=elfcore::ext_m7;
  else if(hit(x,y,600,292,190,70)) bit=elfcore::ext_M7;
  else if(hit(x,y,810,292,190,70)) bit=elfcore::ext_9;
  if(!bit) return;
  g_engine.extensions^=bit;
  release_active_chord();
  draw_instrument();
}

}  // namespace

bool try_unlock(uint32_t frequency_hz,int32_t x,int32_t y,bool pressed,
                screens::Id return_to,uint32_t now_ms) {
  if(g_active) return true;
  const bool eligible=frequency_hz==kUnlockFrequencyHz;
  const bool on_badge=hit(x,y,kBadgeX,kBadgeY,kBadgeW,kBadgeH);
  if(!eligible || !pressed || !on_badge) {
    g_holding=false;
    g_hold_started_ms=0;
    if(!pressed) g_unlock_latched=false;
    return false;
  }
  if(g_unlock_latched) return false;
  if(!g_holding) {
    g_holding=true;
    g_hold_started_ms=now_ms;
    return false;
  }
  if(now_ms-g_hold_started_ms<kUnlockHoldMs) return false;

  g_unlock_latched=true;
  g_holding=false;
  enter(return_to,now_ms);
  return true;
}

bool active() { return g_active && screens::is_active(screens::Id::elf); }

void service_touch(int32_t x,int32_t y,bool pressed) {
  if(!active()) return;

  if(!pressed && g_touch_was_pressed && g_page==Page::instrument && g_active_notes.count) {
    release_active_chord();
    draw_instrument();
  }

  if(pressed && !g_touch_was_pressed) {
    if(hit(x,y,1010,620,230,62)) {
      g_exit_requested=true;
    } else if(g_page==Page::splash && hit(x,y,410,520,460,88)) {
      g_page=Page::instrument;
      draw_instrument();
    } else if(g_page==Page::instrument) {
      select_type(x,y);
      toggle_ext(x,y);
      constexpr int rootY=445, rootW=130, rootH=82, gap=18, startX=145;
      for(int i=0;i<7;++i) {
        if(hit(x,y,startX+i*(rootW+gap),rootY,rootW,rootH)) {
          play_root(i);
          draw_instrument();
          break;
        }
      }
    }
  }

  g_touch_was_pressed=pressed;
}

bool take_exit_request() {
  const bool requested=g_exit_requested;
  g_exit_requested=false;
  return requested;
}

screens::Id begin_leave(uint32_t now_ms) {
  release_active_chord();
  g_synth.all_notes_off();
  const screens::Id restore=
      (g_return_to==screens::Id::elf || g_return_to==screens::Id::none)
          ? screens::Id::radio : g_return_to;
  g_active=false;
  g_holding=false;
  g_unlock_latched=false;
  g_touch_was_pressed=false;
  screens::begin_transition(restore,now_ms);
  return restore;
}

void render_audio(int16_t* mono,size_t frames) { g_synth.render(mono,frames); }
void all_notes_off() { release_active_chord(); g_synth.all_notes_off(); }
size_t active_voice_count() { return g_synth.active_voice_count(); }

bool self_check() {
  return kUnlockFrequencyHz==440000000u &&
         elfcore::self_check() && elfsynth::self_check();
}

}  // namespace orcsdr::elf_mode
