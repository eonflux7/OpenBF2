# Renderer

The `render` lane replaces BF2's forward, gamma-space, lightmap-lit renderer with a linear HDR
pipeline built on a depth prepass and cascaded sun shadows, and later on a shared atmosphere,
image-based ambient specular and screen-space ambient occlusion.
It is a new design. Retail behaviour is not a constraint, and an accepted step leaves no legacy path
behind it.

The engine side lives here, mostly in `Code/BF2/RendDX9/` (`BF2Render.cpp`, `SunShadows.cpp`,
`StateManager.cpp`, `RenderQueues.cpp`, `Ra/EffectManager.cpp`). The shader side is PR's shader set,
kept in a private repository; this document describes only the contract between the two.

## Roadmap

| Step | State | Engine commits |
|---|---|---|
| 1. Reversed-Z | Accepted | `cb37584f`, `d7481b9c` |
| 2. Linear lighting + HDR scene | Accepted | `edc83e55`, `5eb26d06`, `2ee49ec7`, `1c2257b8` |
| 3. Shared lighting model | Accepted (shader-only) | — |
| 4. Depth prepass + screen-space sun mask | Accepted | `3a00bf99`, `988774bf`, `82e04f61` |
| 5. Sun cascaded shadow maps + PCF | Accepted | `271c8d45`, `df9a6a40`, `6261ac82`, `5ccaf770`, `505ca7ab`, `2860a74a` |
| 6. Shader audit and cleanup | Accepted | `ee33008f`, `7f05cc77`, `88d3b7f1`, `52af0169`, `f6fb255a`, `e7623d07`, `dc97ef7b` |
| 7. Particle lighting | Not started | — |
| 8. MSAA with the single-sample prepass | Not started | — |
| 9. Draw submission | Not started | — |
| 10. Terrain in one pass | Not started | — |
| 11. Atmosphere, sky IBL, water reflections | Not started | — |
| 12. HDR bloom + auto-exposure | Not started | — |
| 13. GTAO + lightmap channel ownership | Not started | — |

Each step is accepted in game against the reference views (see [Tooling](#tooling)) before the next
one starts.

Order (set 2026-11-18): particle lighting first, being small and independent. MSAA stays early
because it settles how full-screen terms meet MSAA before steps 11 and 13 add more. Draw
submission comes before terrain and the atmosphere, so the new terrain draw is written once on the
new path and the parameters steps 11–13 add cost nothing per draw. Terrain comes before the
atmosphere, so aerial perspective and sky reflections are written once into a single terrain pass.
Bloom and auto-exposure depend only on the HDR scene and can move earlier if night layers need them
sooner.

**5. Sun CSM.** Accepted on 2026-11-18 with the values it was developed at (400 m, 2048 per
cascade, 500 m caster reach, normal offset 1.5 and depth bias 1.0 texels); the owner chose not to
tune further. They are console variables since (see [Sun shadows](#sun-shadows)). Caster culling per cascade (`505ca7ab`): each cascade draws the casters of its slice
swept toward the sun (see [Sun shadows](#sun-shadows)). On Muttrah (skirmish, one frame each,
different camera positions) caster draws went from 188 / 419 / 1059 / 2348 in cascades 0-3 (4014,
3.4 times the main prepass) to 152 / 221 / 505 / 1263 (2141, 1.9 times); primitives from 800k to
675k. GPU time of the four cascades: 1.4 ms on Muttrah (RTX 2060).

**6. Shader audit and cleanup.** An inventory of the shader set: every effect and header, its
techniques and permutations, which of them a PR session loads, the code each later step replaces,
and the duplicated lighting, fog and Fresnel code that the shared model should own. The inventory
itself lives with the private shaders; this document records only contract changes. Cleanup that
follows from it:
- The toggles the engine fixed when steps 1–4 were accepted (reversed-Z, log depth, gamma-space
  lighting) and their dead branches.
- BF2's directional (sun) shadow maps (done). The directional pass in `XpLightManager.cpp`, its
  records and every receiver are deleted: the mesh shadow permutations (bundled and skinned mesh
  `hasshadow` / `hasshadowocclusion` / `hashemiocclusion`, static mesh `shadow`, the `*Shadowed` tree
  techniques), terrain water's record matrices, undergrowth's `_ds` techniques, overgrowth's record
  attach, and the decal renderer's shadowed batch kinds (decals draw pass 0 only).
- BF2's point and spot shadow maps (done; accepted on The Falklands night layer, 2026-11-18). Retail kept shadow records
  only for lights whose shadow policy was non-zero, and the policy is zero outside the expansion
  pack, so a PR session never drew this path. Deleted: the records, the shadow atlas and its
  device-lost listener, the casters' `DrawShadowMap` draws on bundled, skinned and static meshes
  (with the shadow effects they loaded), the static mesh lightmap silhouette, and the record
  receivers in static meshes, terrain (the shadow-range gather and batch draws, the spot-light pass)
  and overgrowth. The XP light coordinator stays: it still selects the point and spot lights that
  light geometry. The LightManager's shadow vtable slots (`renderShadowMaps`, `registerShadowEffect`),
  the mesh `drawShadowMap` / `drawSilhouette` slots and overgrowth's `collectShadowGeometry` slot
  remain as empty bodies, and the shadow-map console setters only store their value. That pass
  also bound the scene depth-stencil and colour target 0 on exit each frame; the renderer now
  binds them itself where it was called (`dc97ef7b`), and the sun cascades clear their
  standard-depth mark when they restore the scene depth (`e7623d07`).
- One terrain path. `Low_Terrain` had no `RaDepthPrepass` or `RaShadowDepth` technique, so the
  engine no longer enters it: not at load, not from the terrain-quality video option, not as a
  fallback. The `m_lowQuality` branches left in the terrain code are always false and go in step 10.
- Liveness comes from `BF2_DRAW_STATS` ([flags.md](flags.md)), run on a set of maps that between them
  cover water, foliage, buildings and vehicles.

**7. Particle lighting.** Particles (`particles.fx`, `nonscreenalignedparticles.fx`, `trail.fx`,
`meshparticlemesh.fx`) still light in gamma space: they decode the product of vertex colour and
lighting as a colour. They move onto the shared sun and sky model, so smoke, dust and trails match
the scene by day and night. The thermals smoke (paused) builds on this.

**8. MSAA.** The scene draws with 4x MSAA, but the prepass depth, normals and sun mask are single
sample, so an edge pixel reads one mask value for all its samples (halos on silhouettes, mostly
foliage against the sky; on bright terrain, dark lines along ridge silhouettes, where the edge reads the
far slope's N.L and shadow, seen on Kashan Desert). Decide how full-screen terms meet MSAA (edge-aware upsampling,
per-sample evaluation at edges, or another anti-aliasing approach) before step 13 adds a second one.

**9. Draw submission.** The frame is CPU-bound ([frametime.md](frametime.md)): the D3DX effect
framework's per-draw parameter, pass and commit calls take about 15 % of the main thread, and Ra
parameter apply and mesh draws about 14 %. The renderer takes over constant uploads and shader
binding for its draws, keeping the effect files as the shader source. The sun caster pass reuses
the camera's object culling instead of culling again over the cascades' light volume (about 7 %).
Behind a flag selecting the D3DX path until accepted.

**10. Terrain in one pass.** Terrain draws a lightmap accumulation pass, packed to `[0,1]`, and then
additive detail passes (0, 3, 4, 5 and 14 on the maps measured). One pass replaces them: no packing,
fog applied once (the additive passes fog every pass, so it stacks where they overlap), fewer passes
over the largest surface on screen. Roads, which blend into the accumulation target, get their own
lit path. Deleted with it: the always-false `m_lowQuality` branches and the dynamic shadow-range
index buffer (+0x368), which nothing reads since step 6.

**11. Atmosphere, sky IBL, water reflections.**
- One atmosphere model drives the sky dome, the sun's colour and disc, and distance fog with aerial
  perspective, so sky, sun near the horizon and haze agree.
- A sky cubemap prefiltered per level at load (one mip per roughness) gives ambient specular to
  glossy surfaces. Every receiver occludes it: statics and terrain with the lightmap sky visibility,
  vehicles and soldiers with the hemi map, first-person models with the sky visibility under the
  camera. Bundled meshes (vehicles, first-person models) and static meshes come first.
- Material interpretation without new assets: gloss maps become roughness, and each material
  class (paint, glass, metal, fabric) gets its own F0 and roughness range. Vehicle environment
  maps stop being blended into albedo and become specular.
- Water: screen-space reflections. The opaque scene is resolved to a texture before water draws
  (also the source for refraction); water marches against the prepass depth and falls back to the
  prefiltered sky where the ray leaves the screen.

**12. HDR bloom + auto-exposure.** An energy-conserving bloom without threshold, and exposure that
adapts between interiors and open terrain, both before the frame grade. Night layers need it: with
the fixed exposure (−1.25 stops) The Falklands night terrain reaches the tonemap at about 0.0155, and
Khronos Neutral's toe (6.25·x² below 0.08) shows it at 5/255, where retail drew about 30/255. The
adaptation needs an upper limit so night stays dark (about retail's level), and the toe may need
softening for what stays below 0.08.

**13. GTAO + lightmap channel ownership.** GTAO for small-scale occlusion. Lightmaps are kept, with
one owner per channel: inside cascade range the cascades own sun visibility and the baked sun term
is only the fallback beyond the last cascade (as now); sky visibility and baked point lights stay
baked. Lightmaps are not re-baked at a higher resolution unless the reference views show the baked
sky term failing.

Not planned: converting the assets to a PBR material set. The existing maps are interpreted as
PBR inputs (step 11) instead.

### Tooling

Built into the client, not the editor, so that the running game and its scripts are what is measured.

- **GPU pass timers** (`BF2_GPU_TIMERS`, [flags.md](flags.md)): spans in frame order: `pre-world`
  (BeginScene to the sun cascades), `sun clear`, `sun c0`..`sun c3`,
  `depth prepass`, `sun mask`, `world opaque` (to the sky), `world transparent` (fog pass, decals,
  particles, water, transparency), `post pass`, `first person`, `post finish`, `hud` (to Present).
- **Reference views:** per-map lists of camera positions (with or without the first-person model,
  optionally with a spawned vehicle). After autohost the client visits each view, waits a fixed
  number of frames, and writes a screenshot and the pass timings, for regression and performance
  comparison between steps.
- **Debug menu and overlay:** lane `ui`, see [ui.md](ui.md). Pass timings, renderer variables;
  debug views and spawning vehicles and kits come later.

## Frame

In draw order, for the main world view:

1. **Scene target.** From frame prepare to resolve, the scene draws into an `A16B16G16R16F`
   surface (4x MSAA where the back buffer has it) that stands in for the back buffer
   (`rendererSceneColorSurface`). The HUD draws to the real back buffer.
2. **Sun cascades** (`rendererSunShadowsRender`, step 5). Four cascades in a 2 x 2 `R32F` atlas;
   see [Sun shadows](#sun-shadows).
3. **Depth prepass** (`rendererDepthPrepass`), just before the world queues flush, after
   overgrowth has queued. Terrain and every opaque Ra queue entry whose effect has the technique
   `RaDepthPrepass` draw into two single-sample targets with their own `D24S8` depth:
   - COLOR0: linear view depth, `R32F`, 0 where nothing was drawn;
   - COLOR1: world normal, `A2R10G10B10`, `n * 0.5 + 0.5`.
4. **Sun mask.** A full-screen pass (FSQuadDrawer technique `RaShadowMask`) rebuilds each pixel's
   world position from the linear depth and writes an `A8R8G8B8` mask:
   `.r` cascade sun visibility, `.g` the cascades' weight against baked sunlight, `.b` N.L.
5. **World passes.** Lit shaders read the mask instead of computing their own shadows. Water is
   transparent and has no prepass depth, so it samples the cascades itself with `RASM`, which
   `RaShader::begin` sets on every Ra effect that declares it (`rendererSunMaskConstants`).
6. **End of the world pass** (`rendererDepthPrepassEndWorldPass`) binds a 1 x 1 mask of
   `(1, 0, 0)` and zeroes `RASM`, so first-person passes and every other view keep their baked
   sunlight.
7. **Post-production** runs every frame: the colour grade is the frame's only encode from linear
   to display, so it cannot be skipped. Tinnitus blends over the float scene target first.

Depth is reversed throughout (near 1, far 0) except in shadow-map depth buffers.

## Reversed-Z

- Every RenderView projection (perspective, ortho and the linear projection used by the sky dome)
  maps near to 1 and far to 0.
- The StateManager mirrors `ZFUNC` (LESS <-> GREATER) and negates the depth biases while a
  non-shadow depth buffer is bound, re-sending them when the depth buffer changes, and clears
  depth to `1 - z`. Effects keep their forward-Z states.
- XpLightManager marks shadow-map depth buffers as standard depth.
- It needs float depth. DXVK refuses `D24FS8`; `d3d9.useD32forD24 = True` backs `D24S8` with
  `D32_SFLOAT_S8_UINT`.

## Sun shadows

`SunShadows.cpp`. Casters come from a **submission context**: each mesh type's normal submission
takes its cull volume and queue target from a pass context, so the shadow pass re-runs submission
against the cascade volume into a caster list (`rendererShadowCasters`). Camera queues are not
reused, because meshes cull per material against the camera. Casters draw with each Ra effect's
`RaShadowDepth` technique; terrain draws its own (`Terrain::drawShadowCasters`).

Objects come from the object manager's grid query against the light volume without its distance
cull: that cull measures from the frustum's origin and writes each object's cull fade, so against
the light volume it dropped small objects depending on view direction. Hierarchies are expanded
by the drawer, and an object the camera's own distance cull has faded out casts nothing. Seen in game on
Muttrah (`2860a74a`): a supply crate's shadow no longer depends on view direction.

Each cascade draws only the casters that can shadow a receiver in its own slice
(`ShadowCascadeCull`): in light space, the 2D convex hull of the slice's eight corners, pushed out by
the receivers' filter (2.5 cascade texels) plus their normal offset, and in depth from the projection's
sun-side bound to the slice's farthest corner. Casters carry a world bounding sphere; one without
bounds draws in every cascade.

- Four cascades over the view depth up to 400 m, split by a blend of
  logarithmic and uniform (lambda 0.75). Each cascade but the last also covers the first 20 % of
  the next slice; there the receiver blends from the finer cascade to the coarser one, so splits
  don't show.
- Each cascade is a bounding sphere of its view slice, so its size does not change as the camera
  turns, and its centre is snapped to whole texels across the light, so static shadows don't crawl.
- Casters up to 500 m toward the sun beyond the sphere still draw.
- The atlas stores the light-space depth of the nearest caster (0 = sun-side bound, 1 = far side).
  Receivers compare and filter (3 x 3 tent PCF) and, past 90 % of the last split, blend to the
  baked lightmap sun.

Tuned live through console variables, which clamp to their ranges and default to the values the
step was accepted with:

| Variable | Default | Range | What |
|---|---|---|---|
| `renderer.sunShadowDistance` | 400 | 50–2000 m | View depth the cascades reach |
| `renderer.sunShadowResolution` | 2048 | 512–4096 | Texels per cascade side, a power of two; a change re-creates the atlas |
| `renderer.sunShadowCasterReach` | 500 | 0–2000 m | Caster reach toward the sun past a cascade's sphere |
| `renderer.sunShadowSplitLambda` | 0.75 | 0–1 | Splits: 0 uniform, 1 logarithmic |
| `renderer.sunShadowCascadeBlend` | 0.2 | 0–0.5 | Part of the next slice each cascade also covers and blends over |
| `renderer.sunShadowNormalOffset` | 1.5 | 0–4 texels | Receiver offset along its normal |
| `renderer.sunShadowDepthBias` | 1.0 | 0–4 texels | Receiver offset toward the sun in depth |

Their help lines, for the debug menu, come from `rendererVariableHelp` (`RendererConsole.cpp`).

### Mask constants (`RASM`, 22 float4)

| Rows | Contents |
|---|---|
| 0–3 | Camera: position; forward; right / P00; up / P11 |
| 4 + 3i .. 6 + 3i | Cascade i: world to atlas texel (x, y) and light depth |
| 16 | View depth each cascade's slice ends at |
| 17 | `.xyz` direction toward the sun; `.w` view depth where the blend to baked sun starts |
| 18 | Each cascade's texel size in world units |
| 19 | Each cascade's light depth per metre |
| 20 | `.x` one atlas texel in UV; `.y` normal offset and `.z` depth bias, in cascade texels |
| 21 | View depth each cascade's coverage ends at (its blend into the next slice) |

All zero outside the main world pass, and when no cascade was rendered: nothing is covered.

## Shader contract

Samplers 12–15 are reserved for renderer frame resources. No effect may use them for anything else.
Every other sampler states `SRGBTexture` in its `sampler_state`: TRUE where it reads colour (the
hardware decodes before filtering), FALSE for data and outside the scene. D3D9 sampler state carries
over between effects, so neither value is left to default. The engine binds s12–s15 with it FALSE.

| Slot | Resource | Format | Bound |
|---|---|---|---|
| s12 | Prepass world normals | `A2R10G10B10` | After the prepass |
| s13 | Sun cascade atlas | `R32F` | From the cascade pass, rest of the frame |
| s14 | Sun mask | `A8R8G8B8` | World pass; a 1 x 1 `(1, 0, 0)` elsewhere |
| s15 | Linear view depth | `R32F` | After the prepass, rest of the frame |

Techniques the engine looks up by name:

| Technique | In | Draws |
|---|---|---|
| `RaDepthPrepass` | Ra mesh effects, terrain | Linear depth to COLOR0, world normal to COLOR1 |
| `RaShadowDepth` | Ra mesh effects, terrain | Light depth into the atlas, with `RaShadowViewProj` |
| `RaShadowMask` | FSQuadDrawer effect | The sun mask, from `RASM` |

Parameters the engine sets by semantic:

| Semantic | Type | Set on |
|---|---|---|
| `RaShadowViewProj` | `float4x4` | Ra effects drawing `RaShadowDepth`: the cascade being drawn |
| `RASM` | `float4[22]` | The mask pass; in `RaShader::begin` every Ra effect that declares it; the undergrowth effect each frame (grass has no prepass depth and samples the cascades itself) |
| `SUNCOLOR` (`TerrainSunColor` in the Ra constants) | `float3` | Terrain, road and undergrowth effects: the level's one sun, `Lightmanager.staticSunColor` as authored (undergrowth times its `LightingScale`). Retail passed `terrain.sunColor / 4`, balanced against the statics by hand per level |
| `Lights[0].color` (sun record), `SunColor` (decals) | `float3` | Bundled meshes (cockpits included), skinned meshes, trees and decals: the same one sun, `Lightmanager.staticSunColor`. Retail gave them `Lightmanager.sunColor`, the cockpit gate's colour, `treeSunColor` or the sky light's colour, each authored against a separate ambient (on Kashan the dynamic sun is 0.6 against a static 0.73 doubled) |

## Effect cache

Ra effects are cached as `.cfx` files per mod (`Documents/Battlefield 2/mods/<mod>/cache`;
retail shared one cache across mods). A cache entry is keyed by an FNV hash of the preprocessed
source and its defines (`.cfx.key`), so editing an included header invalidates every effect that
includes it. The effect preprocessor loads its own D3DX handle.
