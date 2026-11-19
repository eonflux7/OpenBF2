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

## Next: `render` milestone 9, draw submission (after the benchmark path)

Plan: [renderer.md](renderer.md#9-draw-submission): frame module, submission layer, instancing and
contribution culling, caster culling, contract version.

Measuring: the benchmark paths give the frame time and its spread (before and after, the
acceptance number); the breakdown that steers the work is taken at fixed views, where the frame
holds still: CPU samples by subsystem ([perf-frame.md](perf-frame.md#measuring)), GPU spans and
draws per technique and pass.

Tasks (tasks 1–3 done by an agent, 2026-11-19; task 5 seen by the owner; tasks 4, 6, 7 and 8 loaded by an agent):
1. Breakdown at fixed views: one view each on Muttrah City and Fallujah West (the dense ones),
   CPU samples by subsystem, GPU spans, draws per pass (`BF2_DRAW_STATS`), and the same with the
   sun cascades off, for the caster pass's whole cost. Recorded in
   [perf-frame.md](perf-frame.md#render-milestone-9-task-1). The cascades are 42 % (Muttrah
   `high`, 6.2 of 14.9 ms) and 46 % (Fallujah `city1`, 13.3 of 28.9 ms) of the frame, mostly per
   draw; their own culling is 1 to 2.5 ms. Cascade c3 has three quarters of the caster draws. Off
   switch added for it: `renderer.sunShadows` (`aa754216`, tried by an agent on both maps). Order
   proposed for the owner: 5 (submission layer, prepass and casters) before 4, with one draw per
   mesh in the depth-only passes where materials share the depth shader; contribution culling
   (9) for casters, by size in cascade texels, brought forward next to 4.
2. Contract version: `RA_CONTRACT` passed to every effect compiled; the shaders `#error` when it
   is older than they need. First, since the later tasks change the contract. Done (`2eb902ce`,
   version 1; [shader contract](renderer-pipeline.md#shader-contract)): loaded by an agent on
   Muttrah City, 2026-11-19, all 611 Ra effects recompiled with no errors and the scene drew. The
   `#error` itself not exercised; until the load gate (`tools` milestone 2) exists, a failure shows
   only in the log.
3. Frame module: the pass sequence moves out of `BF2Render.cpp`'s `renderFrame` into its own
   module. No change in what is drawn: view shots before and after match. Done (`0a623df8`):
   `Frame.cpp` owns the scene from the post-production prepare through its finish and the targets
   it draws through; `renderFrame` fills `FrameInputs` and calls `frameRender`. Loaded by an
   agent on Muttrah City, 2026-11-19: views `high` and `market` against the task 1 shots, 22 and
   588 pixels of 1.44 M differ, all on wind-blown palms (PSNR 60 and 44 dB). Not yet seen by
   the owner.
4. Caster culling: the sun caster pass takes the camera's object culling instead of culling again
   over the cascades' light volume. Built as a test at the gather instead (owner's choice,
   2026-11-19: reusing the camera's sets alone would lose the shadows of casters the camera does
   not see): each object from the light volume is tested against the whole shadow range swept
   toward the sun before any work on it, and the casters' spheres are the local box's own.
   `renderer.casterCull` (default 1). Loaded by an agent on Fallujah West and Muttrah City,
   2026-11-19: caster draws per cascade equal with 1 and 0 at five views, captured casters 23 to
   57 % fewer; view shots within the noise; same-session pairs, 1 against 0, Muttrah City `bench1`
   9.59 / 10.73 ms, Fallujah West `bench` 17.85 / 19.62 ms
   ([perf-frame.md](perf-frame.md#render-milestone-9-task-4)). Not yet seen by the owner.
   Proposed next to it (owner, 2026-11-19, "most of the map is static"): cached static casters,
   the far cascades' static casters kept in their map while the snapped cascade holds still,
   moving objects drawn over a copy each frame. Not planned yet.
5. Submission layer and its A/B variable: the packet (shaders, constants, buffers, states), shaders
   and constant layout taken from the effect files once, constants uploaded by the layer. First
   consumers the depth prepass and the sun casters (one technique each, most of the draws).
   Order confirmed by the owner (2026-11-19): 5 before 4. Material depth states by shader contract
   2 (owner's choice over capturing them per material). Written: `Submit.cpp`
   ([Submission](renderer-pipeline.md#submission)), `renderer.submit` (default 1), prshaders
   `83795c6`; the depth passes' order banded by distance, then effect and material (both paths).
   Loaded by an agent on Muttrah City, 2026-11-19: views `high` and `market` with `submit` 1 and 0
   differ in 3 and 102 pixels of 1.44 M (wind-blown palms). `bench1`, 3 runs each: 12.44 ms with
   programs, 12.92 ms through D3DX (13.52 ms before the banded order); profile in
   [perf-frame.md](perf-frame.md#render-milestone-9-task-5). Then float values copied whole and
   register-shaped targets uploaded in place: same-session pairs, 3 runs each, programs against
   D3DX, Muttrah City `bench1` 12.03 / 13.07 ms, Fallujah West `bench` 21.52 / 23.43 ms (8 % on
   both); view `high` with `submit` 1 and 0 differs in 19 pixels. Then the shader constants'
   dirty registers as a bit mask (`f3515fb4`): the pre-draw apply from 8.2 % to 2.5 % of the main
   thread; view `high` differs from the build before in 17 pixels; same-session pairs, programs
   against D3DX, Muttrah City `bench1` 10.86 / 11.73 ms, Fallujah West `bench` 19.40 / 20.78 ms. Then the depth order sorted on keys (`85cf4ae4`; a prefetch of
   the parameter blocks tried and dropped, `f6dd0829`), and one draw per mesh in the depth passes
   (`7bdd3a77`): static mesh index buffers hold whole-template vertex indices, and a run of one
   instance's consecutive opaque materials draws as one range. Loaded by an agent on Muttrah City
   and Fallujah West, 2026-11-19: draws at Muttrah `high` 10541 to 8834; views `high` and `market`
   against the earlier shots differ in 21 and 407 pixels (wind-blown palms); same-session pairs,
   programs against D3DX, Muttrah City `bench1` 10.94 / 11.90 ms, Fallujah West `bench` 18.70 /
   20.52 ms ([perf-frame.md](perf-frame.md#render-milestone-9-task-5)). Seen in game by the
   owner during these runs, 2026-11-19.
6. Static meshes in the lit passes on packets, then bundled and skinned meshes (bone constants).
   Done (`e9b6a8a4`): the queue walks draw a run of one effect and technique through its program,
   so static, bundled and skinned meshes (bones through the `GeomBones` holder), roads, leaves and
   water all leave D3DX; techniques of more than one pass keep it (none was met). Loaded by an
   agent on Muttrah City, Fallujah West and Saaremaa, 2026-11-19: view shots with `submit` 1 and 0
   differ only in wind and smoke; same-session pairs, 1 against 0, Muttrah City `bench1` 10.46 /
   11.78 ms, Fallujah West `bench` 18.39 / 20.26 ms, Saaremaa `bench1` 8.27 / 8.70 ms
   ([perf-frame.md](perf-frame.md#render-milestone-9-task-6)). Not yet seen by the owner.
7. Terrain, overgrowth and undergrowth on packets. Done (`965bb8bf`): terrain and undergrowth
   draw through their engine `Effect`, which they opt in to effect programs (one per pass of a
   technique, [Submission](renderer-pipeline.md#submission)); their parameter writes go to the
   submission layer's value store. Overgrowth was already there: far cells are Ra queue entries
   (task 6), near instances static meshes. Every terrain pass (lit 0, 3, 4, 5, 14, prepass,
   casters, surrounding) and undergrowth `t0_l0` built, none refused. From here on at 1920x1080
   (owner, 2026-11-19). Loaded by an agent, 2026-11-19: view shots with `submit` 1 and 0 differ
   only in wind and water (Muttrah `high` 27, `market` 715; Saaremaa `forest1` 1935, `forest2`
   9460, `runway` 3436 pixels of 2.07 M); same-session pairs, 1 against 0, Muttrah City `bench1`
   9.53 / 10.69 ms, Fallujah West `bench` 18.34 / 21.42 ms, Saaremaa `bench1` 6.71 / 7.09 ms
   (Saaremaa with package builds running on the host: 1 % low spread 105 % at 0)
   ([perf-frame.md](perf-frame.md#render-milestone-9-task-7)). Not yet seen by the owner.
8. Instancing (stream frequency) for overgrowth and repeated static meshes. Measured first
   (`BF2_DRAW_STATS` rows `depth runs`, `depth instance groups`, `lit draws`, `lit instance
   groups`, an agent, 2026-11-19): Muttrah `high` depth passes 5819 draws to 1533 groups, lit
   static walks 2646 to at most 745; Fallujah `city1` depth 12689 to 2065, lit 4482 to 852;
   Saaremaa `forest1` depth 3192 to 524, lit 1654 to 321
   ([perf-frame.md](perf-frame.md#render-milestone-9-task-8)). Design (an agent; shader contract
   3): static meshes always draw through an instance stream (stream 1, one record per instance:
   the world transform's three columns, the lightmap offset, the LOD dither), a group of one
   included, so the prepass and the lit pass compute positions alike; the camera's
   view-projection and the sun (world direction, colour) come from the engine as effect-wide
   parameters in place of the per-instance `WorldViewProjection` and object-space `Lights`; every
   other instance parameter a program reads must be equal across a group (the lightmap texture,
   point lights, transparency). `renderer.instancing` (A/B) 0 draws each instance on its own.
   Static meshes done ([Instancing](renderer-pipeline.md#instancing), prshaders contract 3).
   Loaded by an agent, 2026-11-19: view shots against task 7's within the noise (Muttrah `high`
   59, `market` 916 pixels of 2.07 M; Saaremaa `forest1` 3121, `forest2` 12783, `runway` 4410,
   as task 7's own pairs); Fallujah West `city1` looked at only (no 1920x1080 shot to compare).
   Frame draws: Muttrah `high` 8831 to 3723, Fallujah `city1` 17515 to 4980, Saaremaa `forest1`
   4984 to 4179. Same-session pairs, 1 against 0: Muttrah City `bench1` 8.94 / 10.45 ms,
   Fallujah West `bench` 15.99 / 19.08 ms, Saaremaa `bench1` 6.05 / 6.45 ms (task 7: 9.53,
   18.34, 6.71 ms) ([perf-frame.md](perf-frame.md#render-milestone-9-task-8)). Not yet seen by
   the owner. Then near trees (`leaf`, `trunkstmdetail`/`base`; the overgrowth patches `leafog`,
   `trunkog` are ~100 draws and stay): Saaremaa `forest1` 4157 to 2132 draws (cascades 1583 to
   298); shots match task 8's; separate launches, old shaders against new, Saaremaa `bench1`
   6.50 / 5.96 ms, Fallujah West 16.48 / 16.30, Muttrah City 8.82 / 8.81 (prshaders `d6db632`,
   [perf-frame.md](perf-frame.md#render-milestone-9-task-8)). Not yet seen by the owner. Then
   groups formed through a hash table instead of a sort, and the instance key read in place
   (`3c745649`, `67d04f51`): the depth runs' sort (3.5 % of the main thread at Fallujah `city1`)
   gone, the hash 3.3 to 3.0 % (memory reads, not calls). Loaded by an agent, 2026-11-19: shot
   `city1` differs only on smoke and palms; separate launches, Fallujah West `bench` 16.30 to
   14.79 ms, Muttrah City `bench1` 8.81 to 7.85, Saaremaa `bench1` 5.96 to 5.22. Not yet seen by
   the owner.
9. Contribution culling: below a projected size (`renderer.*` variable) an object is not drawn,
   faded out over the LOD dither band. Done for static meshes (`2002788d`,
   [Contribution culling](renderer-pipeline.md#contribution-culling)): `renderer.contributionCull`,
   default 3 pixels across (owner, 2026-11-19: 8 saves about 10 % but is too eager); vehicles, soldiers and overgrowth left alone. Loaded by an agent,
   2026-11-19: view shots 8 against 0 differ only in wind and smoke (Muttrah `high` 68 pixels,
   Fallujah `city1` 4849, `city3` 9634, Saaremaa `forest1` 430, `runway` 1565 of 2.07 M); same
   session, 2 runs each, 0 against 8: Muttrah City `bench1` 8.08 / 7.51 and 8.08 / 7.20 ms
   (3 pixels: 7.95, 8.08), Fallujah West `bench` 14.67 / 13.21, Saaremaa `bench1` 5.11 / 5.06 and
   5.13 / 5.11 ([perf-frame.md](perf-frame.md#render-milestone-9-task-9)). Not yet seen by the
   owner: the fade and the cull distance show only in motion.
10. Acceptance: the benchmark paths against the baselines above (re-taken at 1920x1080 on a quiet
    host first; the baselines above are 1600x900), view shots matching the D3DX
    path, then the D3DX path and the A/B variable deleted.

Open from milestone 8:
- Crash in the first frame after a device reset: fixed in `717b2671` (a stale RaShadowViewProj
  handle cached by effect pointer). Tried by an agent, Muttrah City, 2026-11-19: 24 resets
  (`renderer.vsync` toggled at a view with the sun cascades drawing) and one `bench.run`, no
  crash; before, about one reset in four crashed. Not yet seen by the owner.
- Frame rate: about 20 % lower on Muttrah City after milestone 8's LOD work (owner), perhaps lower
  again after the overgrowth crossfade (Saaremaa, not measured). CPU-bound; measurements and leads
  in [perf-frame.md](perf-frame.md#after-render-milestone-8). Milestone 9's target.
- Sun shadows: far overgrowth cells cast (`980b32aa`) and the blend to baked goes by camera
  distance (shaders); loaded on Saaremaa, not yet seen.
- The default video profile picks 8x antialiasing (log) while the scene runs 4x. Settle with fixed
  4x (a single-sample HUD back buffer, the scene's own 4x depth; touches device init and reset).
- Roads still fade at 1.3 x `lodSwitchDistance` (130 m); with full-detail terrain they could draw
  farther. Not decided.
