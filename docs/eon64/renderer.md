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
| 10. Terrain in one pass | Next | — |
| 11. Atmosphere, sky IBL, water reflections | Not started | — |
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

Terrain draws a lightmap accumulation pass, packed to `[0,1]`, and then additive detail passes (0,
3, 4, 5 and 14 on the maps measured). One pass replaces them: no packing, fog applied once (the
additive passes fog every pass, so it stacks where they overlap), fewer passes over the largest
surface on screen. Roads, which blend into the accumulation target, get their own lit path. Deleted
with it: the always-false `m_lowQuality` branches and the dynamic shadow-range index buffer
(+0x368), which nothing reads since milestone 6.

Full shading detail at every distance (owner, 2026-11-19): detail layers everywhere and no
low-detail pass, so the `detailNear` split that milestone 8 left in the culler and `drawFinal` goes,
and with it `lodSwitchDistance`. Roads draw to the view distance with no fade (owner, 2026-11-20).
At range the detail textures tile
visibly, so the one pass blends the low-detail colormap in by distance for variation, not instead of
the detail layers.

The compressed resolve, moved here from milestone 8: once terrain is one pass, opaque shaders write
colour compressed by 1 / (1 + luma) through one shared output function, and the resolve output is
expanded back to linear before the transparent pass and post. Water draws in the MSAA scene before
the resolve, so it blends over compressed values; it moves after the expansion or writes in the
same space, decided then.

Found while planning (2026-11-20):
- Today's passes: `drawMain` draws pass 0 (`ZFillLightMap`: depth and the packed lightmap into the
  accumulation target, `+0x774`) per visible node, then the point lights (`drawDetailPass`, pass 16,
  when the effect's shader version is 1, which is past the last pass of `Hi_Terrain` (15);
  otherwise `drawMaskLights`, pass 1). `drawFinal` → `QuadPatch::drawFinal` then draws, per patch,
  each detail layer on its own index buffer (pass 4, 5 for planar "mounten" layers, 11 with the
  environment map, 14 for layer 6) on the `detailNear` nodes, and pass 3 (`LowDiffuse`) on the far
  ones. Each detail pass reads the accumulation target projected (`tex2Dproj`) and fogs.
- Sampler budget: a detail layer needs its detail texture and, when planar, its side texture; six
  layers, their two weight maps, the colormap, lightmap, low-detail texture and low-detail weights,
  the environment cube and the engine's s13 (cascades) and s15 (linear depth) come to more than
  ps_3_0's 16 samplers. The one pass needs another layout (task 2).
- Only terrain and roads read the accumulation target: `rashaderroad.fx`, `roadcompiled.fx` and
  road static meshes (`StaticMesh.cpp`, the `m_isRoad` arm). Once both read lightmaps directly, the
  target and its constants (`raShaderConstantsSetAccumulationMap`, `Frame.cpp`) go. Lightmaps are
  per patch, and a road crosses patches.

Survey (task 1, 2026-11-20; console `terrain.survey` writes it to the log, GPU spans `terrain
lightmap`, `terrain depth`, `terrain detail` and `roads` added in `Frame.cpp`):

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

Patches with one layer and no colormap are sea or off-map (Saaremaa's 34). Every bench run is CPU
bound; terrain's three spans come to 0.8–1.3 ms of GPU.

What this decides:
- No environment-mapped layer and no point light reaches terrain on the three maps: pass 11, the
  environment cube and the light passes (1, 13, 16) have no user to keep in the one pass. Lights
  stay planned as a per-patch constant list (task 3) but are not on the critical path.
- Layer 6 is on most patches of two maps (39 and 56 of 64): it is folded into the one pass, not a
  second pass.
- Detail textures are uniform (512², 7 mips) and come in at most three formats a level: one atlas
  per format, at most 6 entries, plus planar side textures (all share the top texture on these
  maps, "same").
- Weight maps have one mip and two formats; low-detail mode 3 (Fallujah West) is a different
  low-detail layout to carry.

Atlases (task 2, 2026-11-20; `Geom/TerrainAtlas.cpp`, console `terrain.atlasDump` writes them as
`.dds` under `eon64-runs/terrain-atlas/` and logs each record's entry):
- Detail atlases: the records' top and side textures, deduplicated, one atlas per (format, size).
  Each entry's cell is twice the texture and holds it 2x2, shifted half a tile, so the tile sits in
  the middle with half a tile of its own wrapped content around it at every mip; the copy is per
  DXT block, so formats and mips stay exact. The last level keeps an 8-texel tile. The shader wraps
  and samples with the unwrapped gradients: `atlasUv = frac(uv) * scale + offset`,
  `tex2Dgrad(atlas, atlasUv, ddx(uv) * scale, ddy(uv) * scale)`, one `float4` per layer.
  Footprints up to half a tile stay in the cell (16x anisotropic at the 8-texel level reaches the
  neighbour, far enough out that the colormap blend dominates).
- Lightmap atlas: the patches' lightmaps in one texture, column = patch x, row = patch z (patch
  index order), all levels down to 4 texels a cell; smaller lightmaps (Saaremaa's 8x8 sea dummies)
  scaled up by block. Seen continuous across cells on the three maps.
- Built on the terrain's first `update` after a load, because the texture manager creates the
  textures in its post-load sweep, after `Terrain::load`; managed pool, released with the terrain.

| | Muttrah City | Fallujah West | Saaremaa |
|---|---|---|---|
| Detail atlases | DXT5 2048², DXT1 2048² (3 + 3) | DXT1 2048², DXT5 2048² (3 + 3) | DXT1 2048² (3), DXT5 1024² (1), DXT3 2048x1024 (2) |
| Lightmap atlas | DXT1 8192², 9 mips, 44.7 MB | DXT1 4096², 11.2 MB | DXT1 8192², 32 cells scaled up |
| Build time | 11.7 ms | 5.0 ms | 29.9 ms |

Samplers for the one pass: up to three detail atlases (side textures are entries in them; the
layer's atlas index is a constant, a uniform branch picks the sampler), the lightmap atlas, two
weight maps, colormap, low-detail texture, s13 cascades and s15 linear depth: at most ten fixed.

Direct terrain light (task 3, 2026-11-20; `renderer.terrainOnePass`, contract 4): `drawFinal`
binds each patch's lightmap on `TEXLAYER1` in place of the accumulation target, and the lit
passes (3, 4, 5, 11) unpack it at the colormap's UV (centroid lookup, mip from the plain UV) with
`Ra_GetTerrainLight`, the same function undergrowth uses; sun cascades and thermals read its raw
sun term as before. Point lights: `drawMain`'s light passes, still drawn into the target for roads
until task 5, also collect each gathered light per patch, and the lit passes add up to four
(`RaTerrainLightPos`/`Col`, nearest the patch centre). Their irradiance no longer scales with the
sun's luminance (the packed target stored it relative to the sun); it is absolute, as on
undergrowth. Muttrah City at view `high`: the same image within the old target's 8-bit steps.

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

### 12. HDR bloom + auto-exposure

An energy-conserving bloom without threshold, and exposure that adapts between interiors and open
terrain, both before the frame grade. Night layers need it: with the fixed exposure (−1.25 stops)
The Falklands night terrain reaches the tonemap at about 0.0155, and Khronos Neutral's toe
(6.25·x² below 0.08) shows it at 5/255, where retail drew about 30/255. The adaptation needs an
upper limit so night stays dark (about retail's level), and the toe may need softening for what
stays below 0.08. Night particles (milestone 7) are checked here, on The Falklands night layer
(`gpm_cq 32`), once night exposure is settled.

### 13. GTAO + lightmap channel ownership

GTAO for small-scale occlusion. Lightmaps are kept, with one owner per channel: inside cascade
range the cascades own sun visibility and the baked sun term is only the fallback beyond the last
cascade (as now); sky visibility and baked point lights stay baked. Lightmaps are not re-baked at a
higher resolution unless the reference views show the baked sky term failing.
