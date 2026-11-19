# Status

Where each lane stands. Read this first; the Detail column leads to each lane's plan or as-built
document. Update it in the commit that changes a lane's state: a task done, something seen in game,
an open issue added or closed. When a milestone is accepted, its section here moves on to the next.

| Lane | Done | Next | Detail |
|---|---|---|---|
| `render` | Milestones 1–8 accepted | Milestone 9, draw submission, after `tools` milestones 1–3 | [renderer.md](renderer.md#roadmap) |
| `tools` | Planned | Milestone 1, views | [tools.md](tools.md#milestones) |
| `ui` | Milestones 1–4 accepted | — | [ui.md](ui.md#milestones) |
| `perf` (load) | Trace, caches, prefetch, worker compiles; flags retired | — | [perf-load.md](perf-load.md#state) |
| `perf` (frame) | `/O2` plugins, `tailField` table | Parked until render milestone 9 | [perf-frame.md](perf-frame.md#plan) |
| `compat` | See the list | Parked (owner, 2026-11-19); rhooks client features when resumed | [compat.md](compat.md#missing) |
| `fix` | G–O, Q fixed | A, C–F open (packaging, build) | [upstream-fixes.md](upstream-fixes.md) |

## Current: `tools` milestone 1, views

Plan: [tools.md](tools.md#1-views). The view tool comes before render milestone 9 (owner,
2026-11-19), so milestone 9 is measured against reference views and a benchmark path. Milestones
1–3 are cut to what milestone 9 needs; the rest waits in tools milestone 5.

Tasks: not yet broken down. First to find: where the client sets the render camera before culling,
and whether the local server's simulation can be paused (it narrows the shot tolerance; not blocking).

## Next: `render` milestone 9, draw submission

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
