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

Plan: [renderer.md](renderer.md#roadmap) (step 8). Last seen in game: sun shadows in the lit shaders, Kashan Desert, 2026-11-18.

Sub-steps, in order, each seen in game before the next:
- [x] Sampler policy (`28487a35`; seen on Kashan Desert 2026-11-18)
- [x] Sun shadows in the lit shaders, screen-space sun mask deleted, centroid cascade position
  (`65760125`; seen on Kashan Desert 2026-11-18. The dark ridge lines were terrain normals flipped
  toward the camera at crests, not MSAA)
- [ ] Prepass into the MSAA scene depth with EQUAL, centroid UVs, transparent depth rebuild
- [ ] Alpha-to-coverage
- [ ] Thin geometry
- [ ] LOD crossfade
- [ ] Acceptance: GPU timer spans and the reference views

Open: the default video profile picks 8x antialiasing (log) while the scene runs 4x; settle with fixed 4x.

Carried over: The Falklands night-layer particle check moved to step 12.
