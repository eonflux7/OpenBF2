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
| 1. ImGui layer | `ui(imgui)` | Planned |
| 2. Sun shadow tuning as console variables (lane `render`, before step 3) | `render(csm)` | Planned |
| 3. Debug menu and performance overlay | `ui(debug)` | Planned |
| 4. Console frontend | `ui(console)` | Planned |

### 1. ImGui layer

One ImGui context, owned by the renderer, since it owns the device, `Reset` and `Present`.

- **Device:** created with the renderer's device; `ImGui_ImplDX9_InvalidateDeviceObjects` before
  `Reset` (`RendDX9Init.cpp`), re-created after.
- **Drawing:** after the HUD, onto the back buffer, before `Present` (`rendererPresent`). Never into
  the HDR scene target, so exposure and tonemapping do not touch it. The DX9 backend saves and
  restores render state around its draw.
- **Input:** the game reads mouse and keyboard through DirectInput, the mouse in exclusive mode, so
  the Win32 messages the ImGui backend expects do not arrive while playing. While a UI window has
  focus, the mouse goes non-exclusive (as in the main menu), the cursor is shown, and game input is
  not delivered. To decide in this step: whether ImGui takes Win32 messages from the game's window
  procedure or events from the game's input layer, and how that crosses from `BF2.exe` to RendDX9
  without breaking the plugin ABI check.
- **Font:** a TTF scaled to the display height; ImGui's built-in bitmap font is unreadable at
  1440p.
- **Hotkey:** Insert toggles the menu (F1-F8 switch vehicle seats). The key is a console variable.

### 2. Sun shadow tuning (lane `render`)

The values step 5 was accepted with become console variables with the same defaults, so they can be
tuned live: distance (400 m), cascade resolution (2048; the atlas is re-created when it changes),
caster reach (500 m), split lambda (0.75), cascade blend (0.2), normal offset (1.5 texels) and depth
bias (1.0 texel). Each variable carries its range and a help line for the debug menu. Recorded in
[renderer.md](renderer.md) when it lands.

### 3. Debug menu and performance overlay

**Menu** (Insert):

- **Variables:** sliders and checkboxes generated from the console registry, using each variable's
  range and help text. Starts with the `renderer.*` variables, the sun shadow ones first.
- **Commands:** a button per line of a command file read at startup (label and console line, for
  example rcon commands for the local server). The file lives with the mod's private settings, not
  in this repository; without it the section is empty.
- **Overlay:** on or off, saved.

**Overlay**, top right, when on:

- Frames per second and a frame-time graph over the last few seconds: average, 1 % low and worst
  frame, CPU and GPU frame time side by side to show which side bounds the frame.
- GPU time per pass (the spans of [renderer.md](renderer.md#tooling)), from the same timestamp
  queries as `BF2_GPU_TIMERS`, issued only while the overlay is shown.
- Draw calls in the last frame and sun casters per cascade.
- Process memory. VRAM is not shown: D3D9's estimate under DXVK is not a real figure.
- Resolution, MSAA, map and game mode.
- Camera position and heading.
- Sun direction and exposure.

DXVK's HUD is turned off in the launch environment in the same change.

### 4. Console frontend

Replaces the retail drop-down console (`drawConsole`) with an ImGui window over the same backend
(`IO/Console`, the console object registry): autocomplete from the registry with types and help,
history, a scrollable and filterable log, copy and paste. Command parsing, `.con` scripts, rcon and
Python's console calls do not change. The retail console drawing is deleted when the step is
accepted.
