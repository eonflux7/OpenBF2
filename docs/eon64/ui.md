# In-game UI

The `ui` lane: in-game UI this fork owns, built on Dear ImGui (1.92.9, vendored in
`Code/BF2/External/imgui` with its DX9 and Win32 backends; BF2Editor already uses it). Rules:
[AGENTS.md](../../AGENTS.md#what-this-fork-is).

- Fork UI replaces the retail UI it supersedes. Once a step is accepted, the retail path is deleted,
  not kept beside it.
- Hidden UI costs nothing: no ImGui frame, no draw, no input capture, no timer queries.
- The dedicated server never creates the layer. The console backend is unaffected by this lane.
- UI never owns a setting. Anything a widget changes is a console variable, so it can also be set
  from scripts, `BF2_DEVCMD_FILE` and config files, and saved.
- PR-specific content (rcon command lists, admin commands) is never compiled in; it is read from
  files outside this tree.

## Steps

| Step | Commit scope | Status |
|---|---|---|
| 1. ImGui layer | `ui(imgui)` | Accepted (`71545137`, `f4a7fd65`, `6604b184`) |
| 2. Sun shadow tuning as console variables (lane `render`, before step 3) | `render(csm)` | Built, not yet seen in game |
| 3. Debug menu and performance overlay | `ui(debug)` | Built, not yet seen in game |
| 4. Console frontend | `ui(console)` | Planned |

### 1. ImGui layer

`RendDX9/UiLayer.cpp`; the game image's side is `RendMisc/IRendererUi.h` and
`BF2Engine::filterGameInput`.

- **Owner:** one ImGui context in RendDX9, which owns the device, `Reset` and `Present`. It is
  created at the first Present; ImGui is compiled into RendDX9 (`Code/Build/renddx9_sources.txt`).
- **Drawing:** in `rendererPresent`, just before `Present`: after the frame, the HUD and the
  screenshot (`renderer.dumpScreen` captures at `EndScene`), so screenshots never show the UI. Its
  own `BeginScene`/`EndScene` on the back buffer, never the HDR scene target. The DX9 backend's
  state block restores render state; the layer restores the render target, depth surface and sRGB
  states itself, so StateManager's caches stay true. Nothing runs while nothing is shown.
- **Device:** the renderer's lost-device listener (`releaseRendererPathResources`) releases the
  backend's objects; they are re-created on the next draw. `rendererReleaseD3DDevice` shuts the
  layer down.
- **Input, window side:** the layer chains a window procedure in front of the game's. The menu key
  toggles the menu and never reaches the game. While the menu is open, ImGui gets every message,
  and mouse, key and character messages stop there (system keys still pass, so Alt+F4 works).
- **Input, game side:** `BF2.exe` asks the renderer singleton for `IRendererUi` through
  `IBase::queryInterface(IID_IRendererUi)`; no export changes, so the plugin ABI check is
  untouched. While `uiCapturesInput()` is true, `filterGameInput` and `filterPlayerInput` deliver
  nothing (as with the console) and the DirectInput mouse is released, so Windows delivers the
  cursor. The message pump (`BF2.cpp`) re-activates the input devices every frame; meanwhile it
  leaves the mouse out, as it does for the main menu, and takes it back once the menu closes.
- **Cursor:** ImGui draws its own; the game keeps the system cursor hidden.
- **Font:** ImGui's embedded scalable font, 16 px at 1080p, scaled with the back-buffer height
  along with the style's sizes.
- **Hotkey:** `renderer.uiMenuKey`, a Win32 virtual-key code, 45 (Insert) by default. F1-F8
  switch vehicle seats.
- **Not written:** no `imgui.ini` or log next to the game.

### 2. Sun shadow tuning (lane `render`)

The values step 5 was accepted with become console variables with the same defaults, so they can be
tuned live: distance (400 m), cascade resolution (2048; the atlas is re-created when it changes),
caster reach (500 m), split lambda (0.75), cascade blend (0.2), normal offset (1.5 texels) and depth
bias (1.0 texel). Each variable carries its range and a help line for the debug menu. The variables
are listed in [renderer.md](renderer.md#sun-shadows).

### 3. Debug menu and performance overlay

`RendDX9/UiDebug.cpp`. The game image tells the UI the mod directory, map and game mode through
`IUiHost` (`IRendererUi::uiSetHost`, from `BF2Engine`).

**Menu** (the menu key):

- **Performance overlay:** a checkbox for `renderer.uiOverlay`.
- **Commands:** a button per line of `<mod>/settings/eon64-commands.txt`, read when the game hands
  the UI its host: `label = console line`, `#` starts a comment. A button runs its line as if typed
  into the console and entered (the console's access rules, echo and history; `rcon` lines go to
  the server). The file is the owner's, outside this repository; without it the section says where
  it is looked for.
- **Variables:** widgets generated from the console registry for every `renderer.*` set/get
  property: a checkbox for `bool`, a slider for a number with a range, otherwise a text field
  applied with Enter. A widget sets its variable through the registry, as the console does. The
  sun shadow variables come first; the rest have a filter. A tooltip gives the help line
  (`rendererVariableHelp`), the console name, the type and the range.

**Saved:** `<mod>/settings/eon64-ui.con`, console lines run when the game hands the UI its host,
rewritten when `renderer.uiOverlay` changes. Nothing else is written.

**Overlay**, top right, while `renderer.uiOverlay` is on. It takes no input; the system cursor is
left alone.

- Frames per second and frame time (the last 30 frames); over the last 5 seconds the average,
  the 1 % low (the 99th percentile frame) and the worst frame.
- A 5-second graph of frame time (white), CPU time (blue) and GPU time (orange), scaled to hold
  the worst frame, with a line at 16.7 ms. CPU time is the frame without the time `Present`
  blocked; GPU time is the whole frame from the timestamp queries. The larger of the two names
  the side that bounds the frame.
- GPU time per pass (the spans of [renderer.md](renderer.md#tooling)), smoothed. The queries
  are those of `BF2_GPU_TIMERS`, issued only while the overlay is on (or the flag is set).
- Draw calls and primitives in the last frame, and draw calls per sun cascade.
- Process memory: private bytes and working set. VRAM is not shown: D3D9's estimate under DXVK
  is not a real figure.
- Back-buffer size and MSAA, map and game mode.
- Camera position, heading and pitch.
- Sun elevation and azimuth. Exposure is a constant in the shaders until auto-exposure (renderer
  step 9) gives the engine a value to show.

DXVK's HUD is off in the launch environment.

### 4. Console frontend

Replaces the retail drop-down console (`drawConsole`) with an ImGui window over the same backend
(`IO/Console`, the console object registry): autocomplete from the registry with types and help,
history, a scrollable and filterable log, copy and paste. Command parsing, `.con` scripts, rcon and
Python's console calls do not change. The retail console drawing is deleted when the step is
accepted.
