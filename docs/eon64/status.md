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
  - [x] Distance culling removed: objects cull only on the frustum, far plane = view distance
    (`b3e44a8d`; seen on Muttrah City 2026-11-19, the owner checked cover at range)
  - [ ] Dithered crossfade for the remaining mesh and overgrowth LOD switches:
    - [ ] Static meshes (`a7d9e0e1`): both LODs within 10% of a switch distance, complementary
      4x4 Bayer pixels through `RaLodDither` (pixel register c222, set per entry by the queues).
      Loaded on Saaremaa 2026-11-19, effects compile. Owner to check: buildings and trees
      dissolve rather than pop at mid range, no doubled point light, frame rate
    - [x] Overgrowth (`1e7a1c89`, `680d8890`): full/LOD instances dither over 10% of the switch
      distance either side instead of fading through Transparency (lodTimer ease gone); far cells
      dissolve over the last tenth of the overgrowth view distance; near instances and far cells
      share one lighting model (static sun, tree ambient / static sky, no baked per-instance
      shadow). Seen on Saaremaa 2026-11-19 (owner: looks good; frame rate seemed lower)
- [ ] Acceptance:
  - [x] GPU timer spans (Saaremaa 2026-11-19, overview camera over the forest and buildings,
    153 draws, 1600x900 4x, last-second means): depth prepass 1.47 ms, world opaque 1.22, sun
    cascades 1.53, resolve 0.19, depth rebuild 0.05; frame 8.25 ms, of which `hud` (present wait)
    3.33. Step 8's own cost (resolve + rebuild) is about 0.24 ms; the frame is CPU-bound (overlay
    CPU 8.53 / GPU 8.50 ms). A ground view was skipped (owner: would not show much)
  - [ ] Reference views

Open: about 20% lower frame rate on Muttrah City than before step 8's LOD work (owner, 2026-11-19;
when it started is not known). GPU spans there sum to about 5.7 ms of a 12.6 ms frame, the rest is
the `hud` span (present wait), so likely CPU: more draws from full-detail terrain, LOD 0 bundled
meshes, static switch distances x3 and no object distance cull. For the optimization pass.
Owner thought frame rate dropped again after the overgrowth crossfade (Saaremaa, 2026-11-19, not
measured). Lead: `Ra_LodDitherClip` runs unconditionally, so opaque shaders that had no clip
before (static mesh, trunks) lose early depth rejection in their lit pass even when the dither is
(0, 1); a static branch or a separate dithered technique would avoid it.

Open (sun shadows, seen on Saaremaa 2026-11-19): a band without tree shadows between the
near overgrowth radius and the cascade end, and the cascade-to-baked edge sliding as the camera
turned. Fixed, loaded on Saaremaa, not yet seen: far cells cast (`980b32aa`), the blend to baked
goes by camera distance (shaders). The baked overgrowth shadow is dropped
(`680d8890`), seen on Saaremaa 2026-11-19.

Open: the default video profile picks 8x antialiasing (log) while the scene runs 4x; settle with fixed 4x
(single-sample HUD back buffer, the scene's own 4x depth; touches device init and reset).

Terrain shading at full detail at every distance is in step 10 (the one-pass rewrite), not here.
Roads still fade at 1.3 x `lodSwitchDistance` (130 m); now that terrain is full detail they could
draw farther. Not decided.

Carried over: The Falklands night-layer particle check moved to step 12.
