// [UI] The fork's in-game UI, as the game image sees it (docs/eon64/ui.md). Not part of retail BF2.
//
// The UI lives in RendDX9, which owns the device and Present. BF2.exe owns input, so it asks the
// renderer singleton for this interface (IBase::queryInterface(IID_IRendererUi)) and, while the UI
// takes the input, releases the mouse and delivers no game input. No export changes: the plugin
// ABI is untouched.
#pragma once

namespace dice {
namespace hfe {
namespace dx9rend {

// Fork-owned interface id; outside the ranges retail's Ids.h uses.
const unsigned int IID_IRendererUi = 0xE6400001u;

class IRendererUi
{
public:
    // True while a UI window has the mouse and keyboard (the menu is open).
    virtual bool uiCapturesInput() const = 0;

protected:
    ~IRendererUi() {}
};

} // namespace dx9rend
} // namespace hfe
} // namespace dice
