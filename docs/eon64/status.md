# Status

Where each lane stands. Read this first; the Detail column leads to each lane's plan or as-built
document. Update it in the commit that changes a lane's state: a task done, something seen in game,
an open issue added or closed. When a milestone is accepted, its section here moves on to the next.

| Lane | Done | Next | Detail |
|---|---|---|---|
| `render` | Milestones 1–8 accepted | Milestone 9, draw submission, after the `tools` benchmark path | [renderer.md](renderer.md#roadmap) |
| `tools` | Milestone 1: tasks 1–3 | Milestone 3 cut to the benchmark path; milestone 1 tasks 4–5 built | [tools.md](tools.md#milestones) |
| `ui` | Milestones 1–4 accepted | — | [ui.md](ui.md#milestones) |
| `perf` (load) | Trace, caches, prefetch, worker compiles; flags retired | — | [perf-load.md](perf-load.md#state) |
| `perf` (frame) | `/O2` plugins, `tailField` table | Parked until render milestone 9 | [perf-frame.md](perf-frame.md#plan) |
| `compat` | See the list | Parked (owner, 2026-11-19); rhooks client features when resumed | [compat.md](compat.md#missing) |
| `fix` | G–O, Q fixed | A, C–F open (packaging, build) | [upstream-fixes.md](upstream-fixes.md) |

## Current: `tools` milestone 3, benchmark path (cut)

Order (owner, 2026-11-19, replacing "milestones 1–3 first"): views and `view.shot` are enough for
still images; milestone 9's frame-rate loss shows while the camera moves (streaming, LOD switches,
culling), so a repeatable camera path with a frame-time CSV comes next, then render milestone 9.
Plan: [tools.md](tools.md#3-benchmark-path). Left for later: milestone 1 tasks 6–8, milestone 2,
and milestone 3's history file and acceptance check (the spread between runs is still reported).

Tasks (1–2 built; tried on Muttrah City by an agent, 2026-11-19: keys recorded from views, a
hand-edited three-key path played through its middle key, camera handed back at the end; not yet
seen by the owner). Paths also in the Views tab (built; for the owner to check):
1. Paths in the view file: `path NAME` with indented `key TIME pos=… yaw=… pitch=… fov=…` lines,
   parsed and written like views; `path.key NAME TIME` appends a keyframe at the current camera.
2. Playback: `path.play NAME`, `path.stop`; real time, Catmull-Rom position, smoothed angles and FOV.
3. `bench.run PATH [runs] [label]`: a warm-up pass, then the runs with vsync and `game.lockFps` off
   (restored after); per-frame CSV (frame, CPU frame time, GPU busy per pass, draws) and a summary
   (average, 1 % low, worst, per-pass averages, spread between runs, CPU or GPU bound) in
   `<tools.outputDir>/<map>/bench/`.
4. One path each on Muttrah City and Saaremaa.

## Paused: `tools` milestone 1, views

Plan: [tools.md](tools.md#1-views).

Found (2026-11-19):
- Hook: `BF2Render::render` (`Game/Main/BF2Render.cpp`), after `setFarPlane` and before the
  `renderFrame` loop. Everything that culls, picks LODs, streams or sets up cascades reads the view
  after that point. On the spawn screen the game's camera is the player's `MultiPlayerFreeCamera`;
  with no player nothing sets the view, so the override must not depend on one.
- `RenderView`: `setViewMatrix` takes camera-to-world (rows right, up, forward, position; y up,
  left-handed); `setFov` is vertical, radians; `setAdjustedFov` widens for widescreen.
- HUD and spawn menu: both gated by `renderer.drawHud`.
- Pause: `ServerGameLogic::setPause` (game status 4) stops triggers, objects and physics and pauses
  particles; it has no console binding. Renderer time is one input, `renderFrame`'s `time`, so a
  freeze passes 0 there while a view is active.
- Screenshot: `renderer.dumpScreen` reads the back buffer at end of scene, after the HUD and before
  ImGui.

Tasks (1–3 done: seen in game by the owner, 2026-11-19; 4–5 built, not yet seen by the owner; 5
tried on Muttrah City by an agent: a view shot before the HUD and one with `tools.shotHud 1`):
1. Camera override: `view.*` state in a `[TOOLS]` module, applied at the hook; HUD hidden through
   `renderer.drawHud` and restored on leaving; idle is one test.
2. Free fly: `view.fly`, F8, keyboard and mouse read directly, wheel FOV, no collision.
3. View file: parse and minimal-diff write of `settings/eon64-views/<map>.txt`; `view.go`, `save`,
   `delete`, `leave`, `list`, `reload`; `run` lines with variable restore.
4. Views tab in the debug menu.
5. `view.shot`: scene read back before the HUD, PNG plus 960-wide JPEG into `tools.outputDir`.
6. Settle and freeze: pending streaming and crossfades, `tools.settleFrames`,
   `tools.settleTimeout`; renderer time frozen; local server paused through `setPause`.
7. Stamping: engine and shader commits, map, mode, layer, resolution, MSAA, vsync, frame cap,
   non-default `renderer.*` variables.
8. Determinism check: two shots of a view, the difference against a stated tolerance, its sources
   named in the report.

## Next: `render` milestone 9, draw submission (after the benchmark path)

Plan: [renderer.md](renderer.md#9-draw-submission): frame module, submission layer, instancing and
contribution culling, caster culling, contract version. Tasks: not yet broken down.

Open from milestone 8:
- Frame rate: about 20 % lower on Muttrah City after milestone 8's LOD work (owner), perhaps lower
  again after the overgrowth crossfade (Saaremaa, not measured). CPU-bound; measurements and leads
  in [perf-frame.md](perf-frame.md#after-render-milestone-8). Milestone 9's target.
- Sun shadows: far overgrowth cells cast (`980b32aa`) and the blend to baked goes by camera
  distance (shaders); loaded on Saaremaa, not yet seen.
- The default video profile picks 8x antialiasing (log) while the scene runs 4x. Settle with fixed
  4x (a single-sample HUD back buffer, the scene's own 4x depth; touches device init and reset).
- Roads still fade at 1.3 x `lodSwitchDistance` (130 m); with full-detail terrain they could draw
  farther. Not decided.
