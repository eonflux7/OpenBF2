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
| 5. Sun cascaded shadow maps + PCF | In progress, behind `BF2_SUN_SHADOWS` | `271c8d45`, `df9a6a40`, `6261ac82` |
| 6. Shader audit and cleanup | Started 2026-11-17 | — |
| 7. MSAA with the single-sample prepass | Not started | — |
| 8. Atmosphere, sky IBL, water reflections | Not started | — |
| 9. HDR bloom + auto-exposure | Not started | — |
| 10. GTAO + lightmap channel ownership | Not started | — |

Each step is accepted in game against the reference views (see [Tooling](#tooling)) before the next
one starts.

**5. Sun CSM.** Open: tuning of distance, resolution and bias, measured with the GPU pass timers.
Caster culling per cascade is done (`505ca7ab`): each cascade draws the casters of its slice swept
toward the sun (see [Sun shadows](#sun-shadows)). On Muttrah (skirmish, one frame each, different
camera positions) caster draws went from 188 / 419 / 1059 / 2348 in cascades 0-3 (4014, 3.4 times the
main prepass) to 152 / 221 / 505 / 1263 (2141, 1.9 times); primitives from 800k to 675k. No missing
shadows were seen in game.

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
  attach, and the decal renderer's shadowed batch kinds (decals draw pass 0 only). Point and spot
  shadow maps are unchanged.
- One terrain path. `Low_Terrain` had no `RaDepthPrepass` or `RaShadowDepth` technique, so the
  engine no longer enters it: not at load, not from the terrain-quality video option, not as a
  fallback. The `m_lowQuality` branches left in the terrain code are always false and go with the
  terrain rewrite.
- Liveness comes from `BF2_DRAW_STATS` ([flags.md](flags.md)), run on a set of maps that between them
  cover water, foliage, buildings and vehicles.

**7. MSAA.** The scene draws with 4x MSAA, but the prepass depth, normals and sun mask are single
sample, so an edge pixel reads one mask value for all its samples (halos on silhouettes, mostly
foliage against the sky). Decide how full-screen terms meet MSAA (edge-aware upsampling,
per-sample evaluation at edges, or another anti-aliasing approach) before step 10 adds a second one.

**8. Atmosphere, sky IBL, water reflections.**
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

**9. HDR bloom + auto-exposure.** An energy-conserving bloom without threshold, and exposure that
adapts between interiors and open terrain, both before the frame grade.

**10. GTAO + lightmap channel ownership.** GTAO for small-scale occlusion. Lightmaps are kept, with
one owner per channel: inside cascade range the cascades own sun visibility and the baked sun term
is only the fallback beyond the last cascade (as now); sky visibility and baked point lights stay
baked. Lightmaps are not re-baked at a higher resolution unless the reference views show the baked
sky term failing.

Not planned: converting the assets to a PBR material set. The existing maps are interpreted as
PBR inputs (step 8) instead.

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
- **Debug overlay** (Dear ImGui, after the reference views): pass timings, debug views, live shader
  constants, and spawning vehicles and kits.

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
four cascade texels for the receivers' filter and normal offset, and in depth from the projection's
sun-side bound to the slice's farthest corner. Casters carry a world bounding sphere; one without
bounds draws in every cascade.

- Four cascades over the view depth up to `BF2_CSM_DISTANCE` (400 m), split by a blend of
  logarithmic and uniform (lambda 0.75). Each cascade but the last also covers the first 20 % of
  the next slice; there the receiver blends from the finer cascade to the coarser one, so splits
  don't show.
- Each cascade is a bounding sphere of its view slice, so its size does not change as the camera
  turns, and its centre is snapped to whole texels across the light, so static shadows don't crawl.
- Casters up to `BF2_CSM_CASTER_REACH` (500 m) toward the sun beyond the sphere still draw.
- The atlas stores the light-space depth of the nearest caster (0 = sun-side bound, 1 = far side).
  Receivers compare and filter (3 x 3 tent PCF) and, past 90 % of the last split, blend to the
  baked lightmap sun.

### Mask constants (`RASM`, 22 float4)

| Rows | Contents |
|---|---|
| 0–3 | Camera: position; forward; right / P00; up / P11 |
| 4 + 3i .. 6 + 3i | Cascade i: world to atlas texel (x, y) and light depth |
| 16 | View depth each cascade's slice ends at |
| 17 | `.xyz` direction toward the sun; `.w` view depth where the blend to baked sun starts |
| 18 | Each cascade's texel size in world units |
| 19 | Each cascade's light depth per metre |
| 20 | `.x` one atlas texel in UV |
| 21 | View depth each cascade's coverage ends at (its blend into the next slice) |

All zero outside the main world pass, and when no cascade was rendered: nothing is covered.

## Shader contract

Samplers 12–15 are reserved for renderer frame resources. No effect may use them for anything else.

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
| `RASM` | `float4[22]` | The mask pass, and in `RaShader::begin` every Ra effect that declares it |
| `SUNCOLOR` (`TerrainSunColor` in the Ra constants) | `float3` | Terrain, road and undergrowth effects: the level's one sun, `Lightmanager.staticSunColor` as authored (undergrowth times its `LightingScale`). Retail passed `terrain.sunColor / 4`, balanced against the statics by hand per level |

## Effect cache

Ra effects are cached as `.cfx` files per mod (`Documents/Battlefield 2/mods/<mod>/cache`;
retail shared one cache across mods). A cache entry is keyed by an FNV hash of the preprocessed
source and its defines (`.cfx.key`), so editing an included header invalidates every effect that
includes it. The effect preprocessor loads its own D3DX handle.
