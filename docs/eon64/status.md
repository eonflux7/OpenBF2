# Status

What is happening now, lane by lane. Read this first; the links lead to the plan and the as-built
description of each lane. Update it in the same commit that changes a lane's state: tick a
sub-step, record what was seen in game, add or close an open issue. When a step is accepted, its
description moves from the lane's plan to its as-built document and its section here is reset to
the next step.

| Lane | Done | Next | Detail |
|---|---|---|---|
| `render` | Steps 1–8 accepted | Step 9, draw submission | [renderer.md](renderer.md#roadmap) |
| `ui` | Steps 1–4 accepted | — | [ui.md](ui.md#steps) |
| `perf` (load) | Trace, caches, prefetch, worker compiles; flags retired | — | [perf-load.md](perf-load.md#state) |
| `perf` (frame) | `/O2` plugins, `tailField` table | Parked until renderer step 9 | [perf-frame.md](perf-frame.md#plan) |
| `compat` | See the list | rhooks client features | [compat.md](compat.md#missing) |
| `fix` | G–Q fixed | A, C–F open (packaging, build) | [upstream-fixes.md](upstream-fixes.md) |

## Current step: `render` step 9, draw submission

Plan: [renderer.md](renderer.md#roadmap) (step 9). Step 8 (anti-aliasing) accepted on Saaremaa
2026-11-19: crossfades fine, GPU spans recorded, reference views skipped (owner); as built in
[renderer-pipeline.md](renderer-pipeline.md#accepted-steps).

Sub-steps: not yet broken down.

Carried over from step 8:
- Frame rate: about 20% lower on Muttrah City after step 8's LOD work (owner), and perhaps lower
  again after the overgrowth crossfade (Saaremaa, not measured). The frame is CPU-bound: GPU spans
  sum to about 5.7 of 12.6 ms on Muttrah City and 4.9 of 8.25 ms on Saaremaa, the rest is the `hud`
  span (present wait). More draws from full-detail terrain, LOD 0 bundled meshes, static switch
  distances x3 and no distance cull. Step 9's target. GPU-side lead: `Ra_LodDitherClip` runs
  unconditionally, so opaque shaders lose early depth rejection even at dither (0, 1); a static
  branch or a separate dithered technique would avoid it.
- Sun shadows: far overgrowth cells cast (`980b32aa`) and the blend to baked goes by camera
  distance (shaders); loaded on Saaremaa, not yet seen.
- The default video profile picks 8x antialiasing (log) while the scene runs 4x; settle with fixed
  4x (single-sample HUD back buffer, the scene's own 4x depth; touches device init and reset).
- Roads still fade at 1.3 x `lodSwitchDistance` (130 m); with full-detail terrain they could draw
  farther. Not decided. Terrain shading at full detail is step 10.

Carried over: The Falklands night-layer particle check moved to step 12.
