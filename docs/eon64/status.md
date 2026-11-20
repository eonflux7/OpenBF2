# Status

Where each lane stands. Read this first; the Detail column leads to each lane's plan or as-built
document. Update it in the commit that changes a lane's state: a task done, something seen in game,
an open issue added or closed. When a milestone is accepted, its section here moves on to the next.

| Lane | Done | Next | Detail |
|---|---|---|---|
| `render` | Milestones 1–9 accepted | Milestone 10, terrain in one pass: 8 tasks planned | [renderer.md](renderer.md#roadmap) |
| `tools` | Milestone 1: tasks 1–3 | Milestone 3 cut to the benchmark path; milestone 1 tasks 4–5 built | [tools.md](tools.md#milestones) |
| `ui` | Milestones 1–4 accepted | — | [ui.md](ui.md#milestones) |
| `perf` (load) | Trace, caches, prefetch, worker compiles; flags retired | — | [perf-load.md](perf-load.md#state) |
| `perf` (frame) | `/O2` plugins, `tailField` table; per-draw cost taken by render milestone 9 | Parked | [perf-frame.md](perf-frame.md#plan) |
| `compat` | See the list | Parked (owner, 2026-11-19); rhooks client features when resumed | [compat.md](compat.md#missing) |
| `fix` | G–O, Q fixed | A, C–F open (packaging, build) | [upstream-fixes.md](upstream-fixes.md) |

## Current: `tools` milestone 3, benchmark path (cut)

Order (owner, 2026-11-19, replacing "milestones 1–3 first"): views and `view.shot` are enough for
still images; milestone 9's frame-rate loss shows while the camera moves (streaming, LOD switches,
culling), so a repeatable camera path with a frame-time CSV comes next, then render milestone 9.
Plan: [tools.md](tools.md#3-benchmark-path). Left for later: milestone 1 tasks 6–8, milestone 2,
and milestone 3's history file and acceptance check (the spread between runs is still reported).

Tasks (1–3 done: paths, playback, their Views tab section and `bench.run` seen in game by the
owner, 2026-11-19, Muttrah City):
1. Paths in the view file: `path NAME` with indented `key TIME pos=… yaw=… pitch=… fov=…` lines,
   parsed and written like views; `path.key NAME TIME` appends a keyframe at the current camera.
2. Playback: `path.play NAME`, `path.stop`; real time, Catmull-Rom position, smoothed angles and FOV.
3. `bench.run PATH [runs] [label]`: a warm-up pass, then the runs with vsync and the frame cap off
   (restored after); per-frame CSV (frame, CPU frame time, GPU time per pass, draws) and a summary
   (average, 1 % low, worst, per-pass averages, spread between runs, CPU or GPU bound) in
   `<tools.outputDir>/<map>/bench/`. Tried: path `new` (10 s), 3 runs at 11.57 ms (86 fps), spread
   0.84 % on average and 18 % on 1 % low, every frame CPU bound, no GPU result dropped; vsync
   restored; the cap bypassed (`game.lockFps 50` ran at 75 fps); `bench.stop` writes nothing.
4. One path each on Muttrah City and Saaremaa: `bench1` on both (20 s, recorded by the owner,
   2026-11-19, Conquest 64). Saaremaa baseline, 3 runs: 8.12 ms (123 fps), 1 % low 11.75 ms,
   CPU bound, spread 5.26 % on average and 9.14 % on 1 % low (above the 2 % / 5 % target).
   Fallujah West added by the owner (worst map, ~35 fps in play), path `bench`: 25.50 ms
   (39 fps), 1 % low 32.05 ms, CPU bound, spread 2.71 % / 2.85 %; pass spans largest in the
   depth prepass (6.2 ms), world opaque (4.8) and sun cascade 3 (3.7), CPU submission included.
   Muttrah City, `bench1`: 13.37 ms (75 fps), 1 % low 18.48 ms, CPU bound, spread 1.16 % /
   1.83 % (within target); depth prepass 3.2 ms, world opaque 3.0, sun cascade 3 1.2.
   Those draw counts (146, 313, 377) left out every queued mesh; counted since `52868e8c`:
   Muttrah City `bench1` 8927 draws a frame (1 run, 14.00 ms, 2026-11-19); Fallujah West `bench`
   16085 (1 run, 26.11 ms) and Saaremaa `bench1` 4464 (1 run, 8.36 ms), engine `aa754216`,
   2026-11-19.

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

## Next: `render` milestone 10, terrain in one pass

Plan: [renderer.md](renderer.md#10-terrain-in-one-pass), with what the current passes do and the
constraints found while planning. A/B switch while in development: `renderer.terrainOnePass`
(new path on by default once task 4 draws; deleted on acceptance with the old path).

Tasks (planned 2026-11-20; none started):
1. Survey on the three maps: patches by detail-layer count, planar, environment-mapped and layer-6
   (underwater) layers; detail texture sizes and formats; which point-light path runs (pass 16 is
   past the technique's last pass) and how many lights reach terrain; terrain's GPU span and draws
   at the reference views and on the benchmark paths (the baseline). Recorded in renderer.md.
2. Sampler layout (left to the agent by the owner, 2026-11-20): per-level detail and side texture
   atlases built at load (same format and size per atlas, wrap in the shader with `tex2Dgrad` for
   the mips, padded), leaving about 11 samplers fixed; the survey only sizes them. One terrain
   lightmap atlas the same way, so roads look up by world position across patches.
3. Lighting without the accumulation target: the lit pass reads the patch lightmap at its own UV,
   unpacked (no `[0,1]` packing); point lights come from a per-patch constant list (nearest N from
   the culler's sphere gather) instead of an additive pass; sun cascades and thermals as now.
4. The one pass: one draw per visible node over LOD 0, every layer blended by its weight
   (planar and environment-mapped layers by per-layer constants), the colormap and low-detail
   texture blended in by distance for variation, fog once. Terrain depth moves into the depth
   prepass like other entries and the lit pass tests EQUAL. Layer 6 folded in or drawn as a second
   pass on the patches that have it, decided from the survey.
5. Roads' own lit path and no fade (owner, 2026-11-20, "roads are cheap, draw them at all
   times"): road effects and road static meshes read the terrain lightmap by world position
   (task 2's atlas) and light themselves, no longer blending into a target. Compiled roads are
   their own meshes over the terrain, culled at 1.3 x `lodSwitchDistance`
   (`RoadCompiledRenderer::m_viewDistanceScale`) and faded in the shader (`RoadFadeOut`); retail
   faded them because far terrain dropped to lower LODs under them. Terrain is LOD 0 everywhere
   since milestone 8, so roads draw to the view distance: the cull limit and the fade pair go.
   Checked for z-fighting far out and for the cost on Fallujah West.
6. Full detail at every distance: `detailNear`, pass 3 and the far-node path go, and with them
   the texture pop the owner sees on terrain (2026-11-20): each culler node switches between the
   detail layers and the low-detail pass at `lodSwitchDistance`. Any pop left after this is
   looked for elsewhere (mips, streaming). `lodSwitchDistance` has no reader left and goes. GPU
   cost measured on the three maps.
7. Compressed resolve: one shared output function in the opaque shaders writes colour times
   1 / (1 + luma); the resolve output is expanded before the transparents and post. Water: moved
   after the expansion or written in the same space, decided here.
8. Acceptance: A/B shots at the reference views on the three maps (a fogged slope where layers
   overlap, a road, a thermals view), benchmark frame times against milestone 9's
   ([renderer-pipeline.md](renderer-pipeline.md#submission)), terrain GPU span and draws. On the
   owner's acceptance, deleted in one change: the switch and old passes, the accumulation target and
   its constants, `drawMain`'s clear and copy, `drawDetailPass`/`drawMaskLights`, the
   `m_lowQuality`/`m_lowQualityB` branches and the terrain-quality option hook (`BF2Render.cpp`),
   the `+0x368` index buffer with its lock and unlock, and the unused `Hi_Terrain` passes.

Milestone 9 (draw submission) accepted by the owner, 2026-11-20; the D3DX path and
`renderer.submit`, `renderer.instancing` and `renderer.casterCull` deleted. Its
frame-time change is recorded in [renderer-pipeline.md](renderer-pipeline.md#submission). The
deletion loaded by an agent on all three maps, 2026-11-20: shots match the new path's acceptance
shots (wind, smoke and water only), draws unchanged, `bench1`/`bench` 7.94 (runs 2–3; run 1 still
streaming), 14.49 and 5.10 ms, no crash.

Open from milestones 8 and 9:
- Crash in the first frame after a device reset: fixed in `717b2671` (a stale RaShadowViewProj
  handle cached by effect pointer). Tried by an agent, Muttrah City, 2026-11-19: 24 resets
  (`renderer.vsync` toggled at a view with the sun cascades drawing) and one `bench.run`, no
  crash; before, about one reset in four crashed. Not yet seen by the owner.
- Sun shadows: far overgrowth cells cast (`980b32aa`) and the blend to baked goes by camera
  distance (shaders); loaded on Saaremaa, not yet seen.
- The default video profile picks 8x antialiasing (log) while the scene runs 4x. Settle with fixed
  4x (a single-sample HUD back buffer, the scene's own 4x depth; touches device init and reset).
- Roads fading at 130 m: taken into milestone 10, task 5.
- Cached static casters (owner, 2026-11-19, "most of the map is static"): the far cascades' static
  casters kept in their map while the snapped cascade holds still, moving objects drawn over a
  copy each frame. Not planned yet.
