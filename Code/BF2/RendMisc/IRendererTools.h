// [TOOLS] What the view tool (docs/eon64/tools.md) asks of the renderer. Not part of retail BF2.
//
// BF2.exe asks the renderer singleton for this interface (IBase::queryInterface(IID_IRendererTools)),
// as for IRendererUi. No export changes: the plugin ABI is untouched.
#pragma once

namespace dice {
namespace hfe {
namespace dx9rend {

// Fork-owned interface id; outside the ranges retail's Ids.h uses.
const unsigned int IID_IRendererTools = 0xE6400002u;

class IRendererTools
{
public:
    // Requests a shot of the next frame drawn: the back buffer, resolved to one sample, read back
    // either just before the HUD paints (afterHud false: the scene, the sky's second pass and
    // nothing drawn later) or at the end of the scene (afterHud true: with the HUD, without the
    // debug menu and console). Written as pngPath (full size) and jpgPath (jpgWidth wide, aspect
    // kept; full size when the frame is narrower). Paths are absolute; their directory exists.
    // Replaces a request not yet served; null paths cancel it.
    virtual void toolsRequestShot(const char* pngPath, const char* jpgPath, int jpgWidth,
                                  bool afterHud) = 0;
    // 0 while the request waits for its frame, 1 written, -1 failed; the message says what was
    // written (size) or what failed. Valid until the next request.
    virtual int toolsShotState(const char*& message) const = 0;

protected:
    ~IRendererTools() {}
};

} // namespace dx9rend
} // namespace hfe
} // namespace dice
