// [UI] The panels the UI layer draws (docs/eon64/ui.md). Not part of retail BF2.
// Internal to RendDX9: UiLayer.cpp owns the ImGui context and calls these between NewFrame and
// Render; the hooks around Present run every frame and do nothing while nothing is shown.
#pragma once

namespace dice {
namespace hfe {
namespace dx9rend {

class IUiHost;

// UiDebug.cpp: the debug menu and the performance overlay.
void uiDebugSetHost(IUiHost* host);   // reads the settings and command files
IUiHost* uiDebugHost();
bool uiOverlayShown();
void uiDebugFrameStart();      // top of uiLayerPresent: frame timing while the overlay is shown
void uiDebugBeforePresent();   // just before Present
void uiDebugPresented();       // just after Present
void uiDebugDrawMenu();
void uiDebugDrawOverlay();

} // namespace dx9rend
} // namespace hfe
} // namespace dice
