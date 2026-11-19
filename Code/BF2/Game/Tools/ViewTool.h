// [TOOLS] The view tool (docs/eon64/tools.md): a client-side camera override and free fly. Not part
// of retail BF2. While no view is active, the game pays one test of g_viewToolActive per hook.
#pragma once

#include "../../Core/BuildConfig.h"

#if !DEDICATED

#include <list>

namespace dice {
namespace hfe {

class ConsoleObject;
namespace io { class PlayerInput; }
namespace dx9rend { class IRenderView; class IUiViewTool; }

// True while the tool owns the render camera (free fly, or later a view).
extern bool g_viewToolActive;
// True during free fly: the message pump leaves the DirectInput mouse released.
extern bool g_viewToolFlying;

// BF2Render::render, after the game has set the view and the far plane, before the frame is drawn:
// replaces the view's camera and FOV with the tool's.
void viewToolApplyCamera(dx9rend::IRenderView* view);

// BF2Engine::filterPlayerInput, first in the filter chain: during free fly the sample is cleared
// (the tool reads the keyboard and mouse itself). Returns false when the sample was taken.
bool viewToolFilterPlayerInput(io::PlayerInput& input);

// The window's WM_KEYDOWN, outside the menus: tools.flyKey toggles free fly.
void viewToolKeyDown(unsigned int virtualKey);
// The window's WM_MOUSEWHEEL delta: the FOV during free fly.
void viewToolMouseWheel(int delta);

// The views as the debug menu's Views tab reads them (IRendererUi.h).
dx9rend::IUiViewTool* viewToolUi();

std::list<ConsoleObject*>& viewToolConsoleObjects();

} // namespace hfe
} // namespace dice

#endif // !DEDICATED
