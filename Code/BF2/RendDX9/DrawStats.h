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
// Until reset with 0, passes that begin are recorded with "@scope" appended to the technique (the
// sun cascade being drawn, for example).
void drawStatsSetScope(const char* scope);
// Adds `n` to the counter row ("(count)", name): an event count per frame rather than draws.
void drawStatsCount(const char* name, unsigned int n);

// Step 8 LOD measurement: a mesh picks its LOD by `lodDistance` (camera distance over zoom, less
// the bounding radius) against `count` ascending switch distances times `scale`. Counts the
// primitives it would draw were the switch distances x1, x2, x3 and x4, and at LOD 0, as the
// counter rows "lod <kind> x1" .. "lod <kind> full". `primitivesAt(lod)` returns a LOD's count.
template <class PrimitivesAt>
void drawStatsLodScales(int kind, const float* switches, int count, float scale, float lodDistance,
                        PrimitivesAt primitivesAt)
{
    static const char* const names[2][5] = {
        { "lod static x1", "lod static x2", "lod static x3", "lod static x4", "lod static full" },
        { "lod bundled x1", "lod bundled x2", "lod bundled x3", "lod bundled x4", "lod bundled full" },
    };
    for (int s = 0; s < 4; ++s)
    {
        int selected = 0;
        for (int i = 0; i < count; ++i)
            if (static_cast<float>(s + 1) * scale * switches[i] < lodDistance)
                selected = i + 1;
        drawStatsCount(names[kind][s], static_cast<unsigned int>(primitivesAt(selected)));
    }
    drawStatsCount(names[kind][4], static_cast<unsigned int>(primitivesAt(0)));
}

} // namespace dx9rend
} // namespace hfe
} // namespace dice
