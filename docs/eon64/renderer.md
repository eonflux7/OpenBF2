# Renderer

The `render` lane replaces BF2's forward, gamma-space, lightmap-lit renderer with a linear HDR
pipeline built on a depth prepass and cascaded sun shadows, and later on a shared atmosphere,
image-based ambient specular and screen-space ambient occlusion. It is a new design: retail
behaviour is not a constraint, and an accepted milestone leaves no legacy path behind it.

The engine side is mostly in `Code/BF2/RendDX9/` (`Frame.cpp`, `BF2Render.cpp`,
`SunShadows.cpp`, `StateManager.cpp`, `RenderQueues.cpp`, `Ra/EffectManager.cpp`). The shaders are
PR's, kept in a private repository; these documents describe only the contract between the two.

This document is the plan: the roadmap and the milestones still to do. What the renderer does today
is in [renderer-pipeline.md](renderer-pipeline.md).

## Roadmap

| Milestone | State | Engine commits |
|---|---|---|
| 1. Reversed-Z | Accepted | `cb37584f`, `d7481b9c` |
| 2. Linear lighting + HDR scene | Accepted | `edc83e55`, `5eb26d06`, `2ee49ec7`, `1c2257b8` |
| 3. Shared lighting model | Accepted (shader-only) | — |
| 4. Depth prepass + screen-space sun mask | Accepted | `3a00bf99`, `988774bf`, `82e04f61` |
| 5. Sun cascaded shadow maps + PCF | Accepted | `271c8d45`, `df9a6a40`, `6261ac82`, `5ccaf770`, `505ca7ab`, `2860a74a` |
| 6. Shader audit and cleanup | Accepted | `ee33008f`, `7f05cc77`, `88d3b7f1`, `52af0169`, `f6fb255a`, `e7623d07`, `dc97ef7b` |
| 7. Particle lighting | Accepted | `b1549478`, `25319adf`, `7fcdc848`, `423ac5e0`, `0bffa203`; fix `6eb1a727` |
| 8. Anti-aliasing | Accepted | `28487a35`, `65760125`, `f11bdfe3`, `b47ac87f`, `0f10eb41`, `f9086318`, `ee0e7ed8`, `98165f0b`, `b3e44a8d`, `a7d9e0e1`, `1e7a1c89`, `680d8890` |
| 9. Draw submission | Accepted | `2eb902ce`, `0a623df8`, `e87543df`, `f3515fb4`, `7bdd3a77`, `4bc56ff8`, `e9b6a8a4`, `965bb8bf`, `c2f7ef33`, `3c745649`, `2002788d` |
| 10. Terrain in one pass | Accepted | `2eb4d02d`, `c21a012e`, `0d9e15a6`, `57fa2899`, `3b9da48c`, `6fd523bb`, and the deletion ("milestone 10 accepted") |
| 11. Atmosphere, sky IBL, water reflections | In progress | — |
| 12. HDR bloom + auto-exposure | Not started | — |
| 13. GTAO + lightmap channel ownership | Not started | — |

The owner accepts each milestone in game before the next starts. From milestone 9 on that is done
against the `tools` lane's reference views and benchmark paths ([tools.md](tools.md)), with the GPU
pass timers ([renderer-pipeline.md](renderer-pipeline.md#gpu-pass-timers)) and the debug overlay
([ui.md](ui.md)). Each milestone from 9 on also states its frame-time change on the benchmark paths
against the previous milestone's baseline; a regression stands only if the owner accepts it
explicitly, and is recorded with the milestone in [renderer-pipeline.md](renderer-pipeline.md).

Order: draw submission comes before terrain and the atmosphere, so the new terrain draw is written
once on the new path and the parameters milestones 11–13 add cost nothing per draw. Terrain comes
before the atmosphere, so aerial perspective and sky reflections are written once into a single
terrain pass. Bloom and auto-exposure depend only on the HDR scene and can move earlier if night
layers need them sooner.

Not planned: converting the assets to a PBR material set. The existing maps are interpreted as PBR
inputs (milestone 11) instead.

### 10. Terrain in one pass

Accepted by the owner, 2026-11-20; as built in [renderer-pipeline.md](renderer-pipeline.md#terrain).
Survey of the three maps (task 1, 2026-11-20; console `terrain.survey`):

| | Muttrah City | Fallujah West | Saaremaa |
|---|---|---|---|
| Patches (128 m) | 8x8 | 4x4 | 8x8 |
| By layer count 1/2/3/4/5/6/7 | 15/17/1/2/17/5/7 | 0/3/3/2/5/2/1 | 34/0/2/7/14/5/2 |
| With planar / env / layer 6 | 46 / 0 / 39 | 16 / 0 / 4 | 30 / 0 / 56 |
| Detail textures | 6, 512² m7, DXT1/DXT5; detail 0 planar | 6, 512² m7, DXT1/DXT5, all planar | 6, 512² m7, DXT1/DXT3/DXT5; detail 0 planar |
| Weight maps | 256², R5G6B5 or DXT1, 1 mip | same | same |
| Colormap / lightmap / low-detail | 512² DXT1 / 1024² DXT1 / 256² DXT1 | low-detail mode 3, 512² R5G6B5 | 512² DXT1 / 1024² DXT1 (32 patches 8x8 dummies) / 256² DXT1 |
| Point lights on terrain | none (`drawMaskLights` path, shader version 0) | none | none |
| `bench1`/`bench` frame | 10.66 ms | 20.51 ms | 6.34 ms |
| GPU: lightmap / depth / detail | 0.398 / 0.168 / 0.337 ms | 0.490 / 0.200 / 0.623 ms | 0.312 / 0.145 / 0.321 ms |
| GPU: roads | 0.095 ms | 0.102 ms | 0.014 ms |
| Draws a frame: pass 0 / 3 / 4 / 5 / 14 | 12 / 13 / 18 / 3 / 3 | 9 / 6 / – / 24 / – | 18 / 17 / 2 / 1 / 3 (average) |

Patches with one layer and no colormap are sea or off-map (Saaremaa's 34). No environment-mapped
layer and no point light reaches terrain on the three maps. The GPU and frame rows are the old
path, measured with `GPU=1`.

### 11. Atmosphere, sky IBL, water reflections

- One atmosphere model drives the sky dome, the sun's colour and disc, and distance fog with aerial
  perspective, so sky, sun near the horizon and haze agree.
- A sky cubemap prefiltered per level at load (one mip per roughness) gives ambient specular to
  glossy surfaces. Every receiver occludes it: statics and terrain with the lightmap sky visibility,
  vehicles and soldiers with the hemi map, first-person models with the sky visibility under the
  camera. Bundled meshes (vehicles, first-person models) and static meshes come first.
- Material interpretation without new assets: gloss maps become roughness, and each material
  class (paint, glass, metal, fabric) gets its own F0 and roughness range. Vehicle environment
  maps stop being blended into albedo and become specular.
- Specular anti-aliasing in the shared lighting model: roughness widens with the screen-space rate
  of change of the normal, and normal-map mips shorten the normal, which is read back as extra
  roughness (Toksvig). MSAA shades once per pixel, so without this glossy surfaces sparkle.
- Water: screen-space reflections. The opaque scene is resolved to a texture before water draws
  (also the source for refraction); water marches against the prepass depth and falls back to the
  prefiltered sky where the ray leaves the screen.

Decisions (owner, 2026-11-20):
- The painted dome and its cloud layers stay. The atmosphere is fitted to them at load: the dome's
  horizon and zenith colours and the authored fog colour set its density and tint, and the dome's
  horizon blends into the model's haze so terrain meets the sky.
- The authored sun colour (`Lightmanager.staticSunColor`) stays the sunlight at the ground, and lit
  surfaces keep using it. The model works backwards from it to the sunlight above the atmosphere,
  so the sky, the sun disc and the haze agree with it.
- The authored fog end stays the visibility limit (PR's view distance; the far plane and culling
  follow it): aerial perspective reaches full haze there.

D3D9 limits: there is no compute and no rendering into volume textures, so the lookup tables are
pixel passes and the aerial-perspective volume is a 2D atlas of depth slices. The new frame
resources need reserved samplers like s13 and s15; task 1 finds which slots no effect uses.

### 12. HDR bloom + auto-exposure

An energy-conserving bloom without threshold, and exposure that adapts between interiors and open
terrain, both before the frame grade. Night layers need it: with the fixed exposure (−1.25 stops)
The Falklands night terrain reaches the tonemap at about 0.0155, and Khronos Neutral's toe
(6.25·x² below 0.08) shows it at 5/255, where retail drew about 30/255. The adaptation needs an
upper limit so night stays dark (about retail's level), and the toe may need softening for what
stays below 0.08. Night particles (milestone 7) are checked here, on The Falklands night layer
(`gpm_cq 32`), once night exposure is settled.

The compressed (Karis) resolve, moved here from milestone 10 (owner, 2026-11-20), decided by an A/B
once auto-exposure runs: at a window seen from a dark interior and at foliage against the sky. It
pays only where an edge's samples straddle the tonemap's shoulder, which at today's fixed exposure
(−1.25 stops, Neutral linear to about 0.8) is little beyond the sky and specular glints (milestone
11 handles those). Cost found in a first attempt: every scene pixel shader encodes through a flag
in an engine-owned pixel register, the resolve expands in the depth rebuild's full-screen pass, and
every pass blending before the resolve (decals, particles, water, first-person transparents,
additive light passes) blends compressed values, with additive sums clamped at the expansion.

### 13. GTAO + lightmap channel ownership

GTAO for small-scale occlusion. Lightmaps are kept, with one owner per channel: inside cascade
range the cascades own sun visibility and the baked sun term is only the fallback beyond the last
cascade (as now); sky visibility and baked point lights stay baked. Lightmaps are not re-baked at a
higher resolution unless the reference views show the baked sky term failing.
