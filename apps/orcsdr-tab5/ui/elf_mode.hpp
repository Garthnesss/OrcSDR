#pragma once

#include <cstdint>

#include "screen_controller.hpp"

namespace orcsdr::elf_mode {

// Phase 0 deliberately starts only after OrcSDR has completed its normal boot.
// The trigger is the musical A440 Easter egg expressed as 440.000 MHz on the SDR.
constexpr uint32_t kUnlockFrequencyHz = 440000000u;
constexpr uint32_t kUnlockHoldMs = 2000u;

// Existing OrcSDR/M5Unified header badge bounds.  Elf Mode does not create a
// second display/touch driver; it only borrows the already initialized hardware.
constexpr int32_t kBadgeX = 24;
constexpr int32_t kBadgeY = 14;
constexpr int32_t kBadgeW = 88;
constexpr int32_t kBadgeH = 88;

bool try_unlock(uint32_t frequency_hz, int32_t x, int32_t y, bool pressed,
                screens::Id return_to, uint32_t now_ms);
bool active();

// Touch service for the Phase 0 full-screen Elf surface.
void service_touch(int32_t x, int32_t y, bool pressed);
bool take_exit_request();

// Begin the return transition and return the OrcSDR surface that should be
// redrawn after screens::finish_transition().
screens::Id begin_leave(uint32_t now_ms);

bool self_check();

}  // namespace orcsdr::elf_mode
