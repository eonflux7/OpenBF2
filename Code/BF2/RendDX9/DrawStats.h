// [DEV] BF2_DRAW_STATS=<path>: draw counts per effect, technique and pass. Not part of retail BF2.
// Off when the variable is unset: every hook below is one test of drawStatsOn().
#pragma once

#include <string>

namespace dice {
namespace hfe {
namespace dx9rend {

extern bool g_drawStatsOn;   // BF2_DRAW_STATS is set (read when RendDX9 loads)

inline bool drawStatsOn() { return g_drawStatsOn; }

// A pass of `effect` (an effect file or Ra shader name) with `technique` begins; draws until the
// next begin or drawStatsEndPass are counted against it.
void drawStatsBeginPass(const std::string& effect, const std::string& technique, unsigned int pass);
void drawStatsEndPass();
void drawStatsDraw(unsigned int primitives);
void drawStatsFrame();   // after a successful Present

} // namespace dx9rend
} // namespace hfe
} // namespace dice
