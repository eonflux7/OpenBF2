# Status

What is happening now, lane by lane. Read this first; the links lead to the plan and the as-built
description of each lane. Update it in the same commit that changes a lane's state: tick a
sub-step, record what was seen in game, add or close an open issue. When a step is accepted, its
description moves from the lane's plan to its as-built document and its section here is reset to
the next step.

| Lane | Done | Next | Detail |
|---|---|---|---|
| `render` | Steps 1–7 accepted | Step 8, anti-aliasing: sun shadows in the lit shaders next | [renderer.md](renderer.md#roadmap) |
| `ui` | Steps 1–4 accepted | — | [ui.md](ui.md#steps) |
| `perf` (load) | Trace, caches, prefetch, worker compiles; flags retired | — | [perf-load.md](perf-load.md#state) |
| `perf` (frame) | `/O2` plugins, `tailField` table | Parked until renderer step 9 | [perf-frame.md](perf-frame.md#plan) |
| `compat` | See the list | rhooks client features | [compat.md](compat.md#missing) |
| `fix` | G–Q fixed | A, C–F open (packaging, build) | [upstream-fixes.md](upstream-fixes.md) |

## Current step: `render` step 8, anti-aliasing

Plan: [renderer.md](renderer.md#roadmap) (step 8). Last seen in game: centroid lightmap UVs, Muttrah City, 2026-11-18.

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
- [ ] Transparent depth rebuild after the resolve; first settle where first-person models draw,
  since they follow the transparents today and the plan keeps them in the MSAA scene
- [ ] Alpha-to-coverage
- [ ] Thin geometry
- [ ] LOD crossfade
- [ ] Acceptance: GPU timer spans and the reference views

Open: the default video profile picks 8x antialiasing (log) while the scene runs 4x; settle with fixed 4x
(single-sample HUD back buffer, the scene's own 4x depth; touches device init and reset).

Carried over: The Falklands night-layer particle check moved to step 12.
