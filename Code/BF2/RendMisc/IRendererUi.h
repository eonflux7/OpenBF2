// [UI] The fork's in-game UI, as the game image sees it (docs/eon64/ui.md). Not part of retail BF2.
//
// The UI lives in RendDX9, which owns the device and Present. BF2.exe owns input, so it asks the
// renderer singleton for this interface (IBase::queryInterface(IID_IRendererUi)) and, while the UI
// takes the input, releases the mouse and delivers no game input. BF2.exe answers what only the
// game knows through IUiHost. No export changes: the plugin ABI is untouched.
#pragma once

namespace dice {
namespace hfe {
namespace dx9rend {

// Fork-owned interface id; outside the ranges retail's Ids.h uses.
const unsigned int IID_IRendererUi = 0xE6400001u;

// What the game image tells the UI. Strings stay valid until the next call.
class IUiHost
{
public:
    virtual const char* uiModDirectory() const = 0;   // GSModDirectory, for example "mods/pr"
    virtual const char* uiMapName() const = 0;        // empty while no level is loaded
    virtual const char* uiGameMode() const = 0;

protected:
    ~IUiHost() {}
};

class IRendererUi
{
public:
    // True while a UI window has the mouse and keyboard (the menu is open): no game input.
    virtual bool uiCapturesInput() const = 0;
    // True once the UI draws the console: the retail console's typing (updateGameInput and the
    // character ring) is left out; the console window types instead.
    virtual bool uiOwnsConsole() const = 0;
    // True while the menu or the console window is shown: the DirectInput mouse stays released.
    virtual bool uiWantsMouse() const = 0;
    // Once, before the first frame the game drives: the UI then reads its settings files.
    virtual void uiSetHost(IUiHost* host) = 0;

protected:
    ~IRendererUi() {}
};

} // namespace dx9rend
} // namespace hfe
} // namespace dice
