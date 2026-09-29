#include "elf_mode.hpp"

#include <M5GFX.h>
#include <M5Unified.h>

namespace orcsdr::elf_mode {
namespace {

constexpr int32_t kScreenW = 1280;
constexpr int32_t kScreenH = 720;

// Finger-sized for the physical 5-inch 1280x720 Tab5 panel.
constexpr int32_t kReturnX = 430;
constexpr int32_t kReturnY = 586;
constexpr int32_t kReturnW = 420;
constexpr int32_t kReturnH = 82;

constexpr uint16_t kBg = TFT_BLACK;
constexpr uint16_t kPanel = 0x0841;
constexpr uint16_t kCyan = 0x05FF;
constexpr uint16_t kGreen = 0x6FE0;
constexpr uint16_t kMuted = 0x8C71;

bool g_active = false;
bool g_holding = false;
bool g_unlock_latched = false;
bool g_touch_was_pressed = false;
bool g_exit_requested = false;
uint32_t g_hold_started_ms = 0;
screens::Id g_return_to = screens::Id::radio;

bool hit(int32_t x, int32_t y, int32_t bx, int32_t by, int32_t bw, int32_t bh) {
  return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

void text(const char* value, int32_t x, int32_t y, uint16_t color, int size) {
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(color, kBg);
  M5.Display.setTextSize(size);
  M5.Display.drawString(value, x, y);
}

void draw_surface() {
  // M5.Display is the M5Unified-owned M5GFX display instance initialized by
  // OrcSDR during its existing startup sequence. Never call M5.begin() here.
  M5.Display.clearScrollRect();
  M5.Display.fillScreen(kBg);

  M5.Display.drawFastHLine(120, 112, 1040, kCyan);
  M5.Display.drawFastHLine(120, 514, 1040, kCyan);

  text("SIGNAL LOCK", kScreenW / 2, 150, kGreen, 3);
  text("440.000", kScreenW / 2, 226, TFT_WHITE, 6);
  text("UNKNOWN PROTOCOL", kScreenW / 2, 290, kMuted, 2);

  text("ELF", kScreenW / 2, 360, kGreen, 5);
  text("ELFCHORDS", kScreenW / 2, 423, TFT_WHITE, 5);
  text("SECRET INSTRUMENT", kScreenW / 2, 470, kCyan, 2);
  text("PHASE 0  /  SYSTEM ONLINE", kScreenW / 2, 536, kMuted, 2);

  M5.Display.fillRoundRect(kReturnX, kReturnY, kReturnW, kReturnH, 14, kPanel);
  M5.Display.drawRoundRect(kReturnX, kReturnY, kReturnW, kReturnH, 14, kGreen);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(TFT_WHITE, kPanel);
  M5.Display.setTextSize(3);
  M5.Display.drawString("RETURN TO ORCSDR", kReturnX + kReturnW / 2,
                        kReturnY + kReturnH / 2);

  // Geometry guard for future edits; the physical Tab5 target is fixed 1280x720.
  if (M5.Display.width() != kScreenW || M5.Display.height() != kScreenH) {
    M5.Display.setTextDatum(top_left);
    M5.Display.setTextColor(TFT_YELLOW, kBg);
    M5.Display.setTextSize(1);
    M5.Display.drawString("ELF UI TARGET: TAB5 1280x720", 12, 694);
  }
}

void enter(screens::Id return_to, uint32_t now_ms) {
  if (g_active) return;
  g_return_to =
      (return_to == screens::Id::none || return_to == screens::Id::elf)
          ? screens::Id::radio
          : return_to;
  g_exit_requested = false;
  g_touch_was_pressed = true;  // consume the unlock hold
  screens::begin_transition(screens::Id::elf, now_ms);
  g_active = true;
  draw_surface();
  screens::finish_transition();
}

}  // namespace

bool try_unlock(uint32_t frequency_hz, int32_t x, int32_t y, bool pressed,
                screens::Id return_to, uint32_t now_ms) {
  if (g_active) return true;

  const bool eligible = frequency_hz == kUnlockFrequencyHz;
  const bool on_badge = hit(x, y, kBadgeX, kBadgeY, kBadgeW, kBadgeH);

  if (!eligible || !pressed || !on_badge) {
    g_holding = false;
    g_hold_started_ms = 0;
    if (!pressed) g_unlock_latched = false;
    return false;
  }

  if (g_unlock_latched) return false;
  if (!g_holding) {
    g_holding = true;
    g_hold_started_ms = now_ms;
    return false;
  }

  if (now_ms - g_hold_started_ms < kUnlockHoldMs) return false;

  g_unlock_latched = true;
  g_holding = false;
  enter(return_to, now_ms);
  return true;
}

bool active() { return g_active && screens::is_active(screens::Id::elf); }

void service_touch(int32_t x, int32_t y, bool pressed) {
  if (!active()) return;
  if (pressed && !g_touch_was_pressed &&
      hit(x, y, kReturnX, kReturnY, kReturnW, kReturnH)) {
    g_exit_requested = true;
  }
  g_touch_was_pressed = pressed;
}

bool take_exit_request() {
  const bool requested = g_exit_requested;
  g_exit_requested = false;
  return requested;
}

screens::Id begin_leave(uint32_t now_ms) {
  const screens::Id restore =
      g_return_to == screens::Id::elf || g_return_to == screens::Id::none
          ? screens::Id::radio
          : g_return_to;
  g_active = false;
  g_holding = false;
  g_unlock_latched = false;
  g_touch_was_pressed = false;
  screens::begin_transition(restore, now_ms);
  return restore;
}

bool self_check() {
  return kUnlockFrequencyHz == 440000000u && kUnlockHoldMs >= 1500u &&
         kBadgeX >= 0 && kBadgeY >= 0 && kBadgeX + kBadgeW <= kScreenW &&
         kBadgeY + kBadgeH <= kScreenH && kReturnX >= 0 && kReturnY >= 0 &&
         kReturnX + kReturnW <= kScreenW && kReturnY + kReturnH <= kScreenH &&
         kReturnW >= 300 && kReturnH >= 70;
}

}  // namespace orcsdr::elf_mode
