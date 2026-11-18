# Status

What is happening now, lane by lane. Read this first; the links lead to the plan and the as-built
description of each lane. Update it in the same commit that changes a lane's state: tick a
sub-step, record what was seen in game, add or close an open issue. When a step is accepted, its
description moves from the lane's plan to its as-built document and its section here is reset to
the next step.

| Lane | Done | Next | Detail |
|---|---|---|---|
| `render` | Steps 1–7 accepted | Step 8, anti-aliasing: LOD policy and crossfade | [renderer.md](renderer.md#roadmap) |
| `ui` | Steps 1–4 accepted | — | [ui.md](ui.md#steps) |
| `perf` (load) | Trace, caches, prefetch, worker compiles; flags retired | — | [perf-load.md](perf-load.md#state) |
| `perf` (frame) | `/O2` plugins, `tailField` table | Parked until renderer step 9 | [perf-frame.md](perf-frame.md#plan) |
| `compat` | See the list | rhooks client features | [compat.md](compat.md#missing) |
| `fix` | G–Q fixed | A, C–F open (packaging, build) | [upstream-fixes.md](upstream-fixes.md) |

## Current step: `render` step 8, anti-aliasing

Plan: [renderer.md](renderer.md#roadmap) (step 8). Last seen in game: transparent depth rebuild, Muttrah City, 2026-11-18. Thin geometry dropped
(owner, 2026-11-18; why: [renderer.md](renderer.md#roadmap)).

Sub-steps, in order, each seen in game before the next:
- [x] Sampler policy (`28487a35`; seen on Kashan Desert 2026-11-18)
- [x] Sun shadows in the lit shaders, screen-space sun mask deleted, centroid cascade position
  (`65760125`; seen on Kashan Desert 2026-11-18. The dark ridge lines were terrain normals flipped
  toward the camera at crests, not MSAA)
- [x] Prepass into the MSAA scene depth, prepassed draws EQUAL with no depth write, 4x linear
  depth resolved to s15 (`f11bdfe3`; seen on Kashan Desert and Saaremaa 2026-11-18, forest,
  soldiers and vehicles by the owner)
- [x] Centroid lightmap UVs, mip derivatives from a non-centroid copy (static mesh and terrain's
  lightmap pass; roads have no lightmap UV, they read terrain's resolved accumulation in screen
  space. Seen on Muttrah City 2026-11-18)
- [x] Transparent depth rebuild after the resolve (`b47ac87f`, `0f10eb41`). First-person models
  draw into the 4x scene before the resolve, with a prepass of their own (the owner's call,
  2026-11-18); water adds its linear depth; the world transparents draw single sample against the
  rebuilt depth. Seen on Muttrah City 2026-11-18. Expected: smoke now draws over first-person
  transparent surfaces (scope glass)
- [x] Alpha-to-coverage (`f9086318`: the mesh prepass runs under ATOC, prepassed lit passes drop
  their alpha test; prepass alpha sharpened to a one-pixel ramp. Seen on Saaremaa 2026-11-18.
  Not done: the mip-loss alpha correction, which needs texture sizes from the engine; add it if
  distant foliage thins)
- [ ] LOD policy and crossfade ([renderer.md](renderer.md#roadmap), step 8), in order:
  - [x] Full-detail terrain geometry (`ee0e7ed8`; seen on Kashan Desert 2026-11-19, no cracks.
    Owner to check: ridges scoped and unscoped, no ground sliding, frame rate)
  - [x] Mesh LOD distances raised; mesh shading LODs removed; full-resolution textures confirmed
    (`98165f0b`: bundled meshes always LOD 0, static switch distances x3, shader-quality buckets
    deleted; textures load at full resolution at texture quality 3. Loaded on Muttrah City
    2026-11-19. Owner to check: no visible building pops at mid range, vehicles at range, frame rate)
  - [ ] Distance culling: cover no earlier than what it hides
  - [ ] Dithered crossfade for the remaining switches and the cull fade
- [ ] Acceptance: GPU timer spans and the reference views

Open: the default video profile picks 8x antialiasing (log) while the scene runs 4x; settle with fixed 4x
(single-sample HUD back buffer, the scene's own 4x depth; touches device init and reset).

Terrain shading at full detail at every distance is in step 10 (the one-pass rewrite), not here.
Roads still fade at 1.3 x `lodSwitchDistance` (130 m); now that terrain is full detail they could
draw farther. Not decided.

Carried over: The Falklands night-layer particle check moved to step 12.
