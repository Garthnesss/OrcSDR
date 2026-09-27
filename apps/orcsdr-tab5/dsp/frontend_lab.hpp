#pragma once

#include <cstddef>
#include <cstdint>

// Stage-2 frontend benchmark lab (built only with ORCSDR_DSP_LAB=1).
// Nothing here feeds audio: shadow mode runs a candidate on live IQ blocks
// inside the DSP task and discards the output, only to measure timing.
namespace orcsdr::dsp::lab {

using Emit = void (*)(const char* line);

// "RTL_DSP LAB <args>". radio_running tells offline jobs to refuse.
// Offline jobs (BENCH/TEST/SWEEP/CONT/PREEMPT) run in a lab task on core 1
// and print LAB_* lines; the call returns immediately.
void command(const char* args, bool radio_running, Emit emit);

// DSP task hooks. A successful exclusive live block replaces legacy demod only in lab builds.
bool live_active();
bool shadow_block(const uint8_t* cu8, size_t bytes, uint32_t sample_rate);
void shadow_total(uint32_t block_us);

}  // namespace orcsdr::dsp::lab
