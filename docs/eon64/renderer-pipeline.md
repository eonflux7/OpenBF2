# Renderer pipeline

What the `render` lane's renderer does today: the accepted roadmap steps, the frame in draw order,
and the contract between the engine and the (private) shaders. The plan and the steps still to do
are in [renderer.md](renderer.md). Update this document when a step is accepted.

## Accepted steps

Steps 1–4 (reversed-Z, linear lighting and the HDR scene, the shared lighting model, the depth
prepass and sun mask) are described by the sections below. Commits are listed in the
[roadmap](renderer.md#roadmap).

**5. Sun CSM.** Accepted on 2026-11-18 with the values it was developed at (400 m, 2048 per
cascade, 500 m caster reach, normal offset 1.5 and depth bias 1.0 texels); the owner chose not to
tune further. They are console variables since (see [Sun shadows](#sun-shadows)).
Caster culling per cascade (`505ca7ab`): each cascade draws the casters of its slice swept toward
the sun (see [Sun shadows](#sun-shadows)). On Muttrah (skirmish, one frame each,
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

**7. Particle lighting.** The four particle effects (`particles.fx`, `nonscreenalignedparticles.fx`,
`trail.fx`, `meshparticlemesh.fx`) light in linear on the level's one sun, through one shared
function in the shaders. Every particle renderer calls `rendererBindParticleLighting` as it binds its
effect (sun, sky, sun direction, `RASM`, the cascade atlas, soft-particle constants; see the
[shader contract](#shader-contract)).
- Only the authored tint is decoded; light multiplies it in linear. Lit tints are capped at 1 (an
  albedo): PR's tints above 1 compensated for its dim gamma-space sun and glowed once decoded.
- Lighting is per vertex. A billboard lights at its centre, with one cascade lookup however much it
  overdraws; beyond the cascades, and outside the main world pass, the baked term (hemi map alpha
  plus the template's altitude offset) is the sun visibility. Billboards and trails have no normal:
  full sun plus the hemisphere's average, whose ground bounce takes the hemi map's colour as static
  meshes do. Mesh particles use their normal.
- Emissive: additive effects, and the unlit part of a template's light factor, show their tint
  times 2.38 (2^1.25, undoing the frame's exposure, so they reach the tonemap at their authored value).
- A template that sets no light factor is lit fully. Retail gave it the level's
  `defaultEffectLightAffectionFactor`, which eight PR levels set to 0 (Kashan, Adak, Burning Sands,
  Icebreaker, Iron Ridge, Khamisiyah, Masirah, Road to Damascus; Fool's Road 0.7), so their effects
  drew unlit and flat. Templates that set a factor keep it.
- Soft particles: lit and additive particles fade over `renderer.softParticleDistance` (default
  1.5 m, range 0.1–20) in front of the prepass depth (s15). Alpha-blended passes fade alpha, `ONE,ONE`
  passes fade colour; `DiffuseWithZWrite` stays hard. No fade on the sky (depth 0) or outside the
  main world pass (first person, other views).
- Screen-aligned sprites turn against the camera's roll, so smoke stays upright when an aircraft rolls.
- Thermals keep PR's particle output exactly: the old gamma-space formula with `EffectSunColor` /
  `EffectShadowColor`, the level's light factor, hard edges. The thermals smoke (paused) replaces it.

Limits: a billboard or trail lights at one point, so a streak that starts in shade stays shaded where
it rises into the sun. Water and undergrowth draw no prepass depth, so particles don't soften against
them; a fringe at depth edges remains, and after step 8 it follows the averaged resolve depth
instead (see step 8). Fire reads red-orange rather than yellow-orange:
the PBR Neutral tonemap keeps hue as it compresses highlights, where PR's per-channel clip pushed
bright orange toward yellow (AgX was tried and not preferred). That is the grade's, not this step's.


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
   sunlight, and stops soft particles.
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

Every `sampler_state` also states its filter. A material sampler (a colour, normal or detail texture
mapped onto world geometry) is 16x anisotropic: min `ANISOTROPIC`, mag and mip `LINEAR`. A data
sampler (lightmaps, light and blend maps, cube and lookup maps, render targets, UI) states point or
linear. The engine defines no filter: retail's caps- and setting-dependent `FILTER_*` defines and
`IS_NV4X` are gone, so the texture filter quality setting changes nothing and the effect cache
directory no longer carries it (`_3153_0`).

| Slot | Resource | Format | Bound |
|---|---|---|---|
| s12 | Prepass world normals | `A2R10G10B10` | After the prepass |
| s13 | Sun cascade atlas | `R32F` | From the cascade pass, rest of the frame |
| s14 | Sun mask | `A8R8G8B8` | World pass; a 1 x 1 `(1, 0, 0)` elsewhere |
| s15 | Linear view depth | `R32F` | After the prepass, rest of the frame; particles read it only while `RaSoftParticles.x` > 0 |

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
| `RASM` | `float4[22]` | The mask pass; in `RaShader::begin` every Ra effect that declares it; the undergrowth effect each frame (grass has no prepass depth and samples the cascades itself); the particle effects at every bind (`rendererBindParticleLighting`) |
| `SUNCOLOR` (`TerrainSunColor` in the Ra constants) | `float3` | Terrain, road and undergrowth effects: the level's one sun, `Lightmanager.staticSunColor` as authored (undergrowth times its `LightingScale`). Retail passed `terrain.sunColor / 4`, balanced against the statics by hand per level |
| `RaSunColor`, `RaSkyColor` | `float3` | The four particle effects (`rendererBindParticleLighting`, called by each particle renderer as it binds its effect): `Lightmanager.staticSunColor` and `staticSkyColor` as authored, the one sun and sky every lit surface uses |
| `RaSunDirection` | `float3` | The particle effects: unit vector toward the sun (`-Lightmanager.sunDirection`) |
| `RaSunCascadeAtlas` | `texture` | The particle effects: the cascade atlas, which they sample per vertex. A vertex shader cannot read s13, so the atlas reaches them as a texture parameter and D3DX binds the vertex sampler |
| `RaSoftParticles` | `float4` | The particle effects: `.x` 1 / `renderer.softParticleDistance` while the main world pass draws over this frame's prepass depth, else 0 (no fade); `.zw` 1 / the depth target's size |
| `TemplateParameters` `color1.a` (light factor) | `float` | The particle templates, as retail, except a template that sets no light factor: `-1 - Lightmanager.defaultEffectLightAffectionFactor` (`particleTemplateUnsetLightAffection`). The shaders light it fully; thermals read back the level default |
| `EffectSunColor`, `EffectShadowColor` | `float3` | The particle effects, as retail: read only by the thermals path, which keeps PR's particle formula, and (`EffectSunColor`) as the fill scale of the `ShowFill` techniques |
| `Lights[0].color` (sun record), `SunColor` (decals) | `float3` | Bundled meshes (cockpits included), skinned meshes, trees and decals: the same one sun, `Lightmanager.staticSunColor`. Retail gave them `Lightmanager.sunColor`, the cockpit gate's colour, `treeSunColor` or the sky light's colour, each authored against a separate ambient (on Kashan the dynamic sun is 0.6 against a static 0.73 doubled). Unscaled: retail also multiplied bundled and skinned meshes' sun by the baked simple shadow map, which the cascades replace and which blacked out meshes under roofs once the sky came from the sun |

## Effect cache

Ra effects are cached as `.cfx` files per mod (`Documents/Battlefield 2/mods/<mod>/cache`;
retail shared one cache across mods). A cache entry is keyed by an FNV hash of the preprocessed
source and its defines (`.cfx.key`), so editing an included header invalidates every effect that
includes it. The effect preprocessor loads its own D3DX handle.
