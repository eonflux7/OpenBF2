// [DEV] BF2_GPU_TIMERS=<path>: GPU time per renderer pass. Not part of retail BF2.
// Off when the variable is unset: every hook below is one test of gpuTimersOn().
#pragma once

namespace dice {
namespace hfe {
namespace dx9rend {

extern bool g_gpuTimersOn;   // BF2_GPU_TIMERS is set (read when RendDX9 loads)

inline bool gpuTimersOn() { return g_gpuTimersOn; }

// Starts the span `name` (a string literal): the GPU time from here to the next mark, or to the
// end of the frame, is booked to it. A name marked more than once in a frame adds up.
void gpuTimerMark(const char* name);
void gpuTimersFrame();     // after a successful Present: closes the frame, collects older ones
void gpuTimersRelease();   // before the device is reset or released

} // namespace dx9rend
} // namespace hfe
} // namespace dice
