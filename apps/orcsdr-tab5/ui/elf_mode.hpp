#pragma once

#include <cstddef>
#include <cstdint>

#include "screen_controller.hpp"

namespace orcsdr::elf_mode {

constexpr uint32_t kUnlockFrequencyHz = 440000000u;
constexpr uint32_t kUnlockHoldMs = 2000u;

constexpr int32_t kBadgeX = 24;
constexpr int32_t kBadgeY = 14;
constexpr int32_t kBadgeW = 88;
constexpr int32_t kBadgeH = 88;

bool try_unlock(uint32_t frequency_hz, int32_t x, int32_t y, bool pressed,
                screens::Id return_to, uint32_t now_ms);
bool active();

void service_touch(int32_t x, int32_t y, bool pressed);
bool take_exit_request();
screens::Id begin_leave(uint32_t now_ms);

void render_audio(int16_t* mono, size_t frames);
void all_notes_off();
size_t active_voice_count();

bool self_check();

}  // namespace orcsdr::elf_mode
