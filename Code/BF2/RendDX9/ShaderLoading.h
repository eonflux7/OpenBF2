// Renderer loading-screen progress shared by the two effect compilers.
#ifndef BF2_RENDDX9_SHADERLOADING_H
#define BF2_RENDDX9_SHADERLOADING_H

#include "../Core/LoadStat.h"
#include "../Core/EventManager.h"
#if !defined(_MSC_VER)
#include "../Platform/X87Compat.h"
#endif
#include <chrono>
#include <cstdlib>

namespace dice { namespace hfe { namespace dx9rend {

// PE 0x10022310; also inlined at 0x10035836..0x10035891.
inline float shaderLoadingPercent(LoadStat* stat)
{
    if (stat->m_unknown0B)
        return 100.0f;
    const unsigned int total = static_cast<unsigned int>(stat->m_unknown24);
    if (total && static_cast<unsigned int>(stat->m_loadCounter) >= total)
        stat->m_loadCounter = stat->m_unknown24;
#if defined(_MSC_VER)
    if (!total)
        return stat->m_unknown38 * 100.0f;
    return (static_cast<float>(static_cast<unsigned int>(stat->m_loadCounter)) /
            static_cast<float>(total) * (stat->m_unknown3C - stat->m_unknown38) +
            stat->m_unknown38) * 100.0f;
#else
    // [CHOSEN, not recovered] Retain the PE's x87 intermediates on native processors.
    using namespace bf2x87;
    if (!total)
        return static_cast<float>(mul(wide(stat->m_unknown38), wide(100.0f)));
    return static_cast<float>(mul(add(mul(
        div(wide(static_cast<unsigned int>(stat->m_loadCounter)), wide(total)),
        sub(wide(stat->m_unknown3C), wide(stat->m_unknown38))),
        wide(stat->m_unknown38)), wide(100.0f)));
#endif
}

// [PERF] The repaint throttle. Each effect load posts the loading-screen update (ECMenu 4) and
// the menu repaints the loading screen for it, about 1 ms each: 1.15 s for the 1,116 effects of
// the level's geometry part. The load callback, which posts the same update for everything else,
// is throttled to thirty per second (GameLogic::beginLoadStats); this applies the same spacing to
// the effect updates. The percentage is still computed (and the counter clamped) every time, and
// the next update or the load callback paints the current value. BF2_PERF_FXEVENT=0 posts every
// update.
inline bool shaderLoadingEventDue()
{
    static int on = -1;
    if (on < 0) { const char* e = std::getenv("BF2_PERF_FXEVENT"); on = (e && *e == '0') ? 0 : 1; }
    if (!on)
        return true;
    typedef std::chrono::steady_clock Clock;
    static Clock::time_point last;
    static bool posted = false;
    const Clock::time_point now = Clock::now();
    if (posted && now - last < std::chrono::microseconds(33333))
        return false;
    posted = true;
    last = now;
    return true;
}

// PE 0x1002308D..0x100230B2 and 0x1003582C..0x100358A6.
inline void shaderLoadingEvent()
{
    if (loadStat->m_loadCallback)
    {
        float percent = shaderLoadingPercent(loadStat);
        if (shaderLoadingEventDue())   // [PERF] the repaint throttle
            eventManager->postEvent(static_cast<EventCategory>(10), 4, &percent, 0.0f);
    }
}

} } }
#endif
