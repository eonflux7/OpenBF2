# Renderer

The `render` lane replaces BF2's forward, gamma-space, lightmap-lit renderer with a linear HDR
pipeline built on a depth prepass and cascaded sun shadows, and later on a shared atmosphere,
image-based ambient specular and screen-space ambient occlusion.
It is a new design. Retail behaviour is not a constraint, and an accepted step leaves no legacy path
behind it.

The engine side lives here, mostly in `Code/BF2/RendDX9/` (`BF2Render.cpp`, `SunShadows.cpp`,
`StateManager.cpp`, `RenderQueues.cpp`, `Ra/EffectManager.cpp`). The shader side is PR's shader set,
kept in a private repository; this document describes only the contract between the two.

This document is the plan: the roadmap, the order and the steps still to do. What the renderer
does today, and the engine/shader contract, are in [renderer-pipeline.md](renderer-pipeline.md).

## Roadmap

| Step | State | Engine commits |
|---|---|---|
| 1. Reversed-Z | Accepted | `cb37584f`, `d7481b9c` |
| 2. Linear lighting + HDR scene | Accepted | `edc83e55`, `5eb26d06`, `2ee49ec7`, `1c2257b8` |
| 3. Shared lighting model | Accepted (shader-only) | — |
| 4. Depth prepass + screen-space sun mask | Accepted | `3a00bf99`, `988774bf`, `82e04f61` |
| 5. Sun cascaded shadow maps + PCF | Accepted | `271c8d45`, `df9a6a40`, `6261ac82`, `5ccaf770`, `505ca7ab`, `2860a74a` |
| 6. Shader audit and cleanup | Accepted | `ee33008f`, `7f05cc77`, `88d3b7f1`, `52af0169`, `f6fb255a`, `e7623d07`, `dc97ef7b` |
| 7. Particle lighting | Accepted | `b1549478`, `25319adf`, `7fcdc848`, `423ac5e0`, `0bffa203`; fix `6eb1a727` |
| 8. Anti-aliasing | Accepted | `28487a35`, `65760125`, `f11bdfe3`, `b47ac87f`, `0f10eb41`, `f9086318`, `ee0e7ed8`, `98165f0b`, `b3e44a8d`, `a7d9e0e1`, `1e7a1c89`, `680d8890` |
| 9. Draw submission | Not started | — |
| 10. Terrain in one pass | Not started | — |
| 11. Atmosphere, sky IBL, water reflections | Not started | — |
| 12. HDR bloom + auto-exposure | Not started | — |
| 13. GTAO + lightmap channel ownership | Not started | — |

Each step is accepted in game against the reference views (see [Tooling](#tooling)) before the next
one starts.

Order (set 2026-11-18): particle lighting first, being small and independent. Anti-aliasing stays
early because it moves sun shadows into the lit shaders and settles the prepass, which steps 11 and
13 build on. Draw
submission comes before terrain and the atmosphere, so the new terrain draw is written once on the
new path and the parameters steps 11–13 add cost nothing per draw. Terrain comes before the
atmosphere, so aerial perspective and sky reflections are written once into a single terrain pass.
Bloom and auto-exposure depend only on the HDR scene and can move earlier if night layers need them
sooner.

What each accepted step changed is described in [renderer-pipeline.md](renderer-pipeline.md#accepted-steps).

**9. Draw submission.** The frame is CPU-bound ([perf-frame.md](perf-frame.md)): the D3DX effect
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
Full shading detail at every distance (the owner's call, 2026-11-19): detail layers everywhere and
no low-detail pass, so the `detailNear` split that step 8 left in the culler and `drawFinal` goes,
with `lodSwitchDistance` reduced to the roads' view distance. At range the detail textures tile
visibly, so the one pass blends the low-detail colormap in by distance for variation, not instead of
the detail layers.
The compressed resolve moved here from step 8: once terrain is one pass, opaque shaders write
colour compressed by 1 / (1 + luma) through one shared output function, and the resolve output is
expanded back to linear before the transparent pass and post. Water draws in the MSAA scene before
the resolve, so it blends over compressed values; it moves after the expansion or writes in the
same space, decided then.

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
- Specular anti-aliasing in the shared lighting model: roughness widens with the screen-space rate
  of change of the normal, and normal-map mips shorten the normal, which is read back as extra
  roughness (Toksvig). MSAA shades once per pixel, so without this glossy surfaces sparkle.
- Water: screen-space reflections. The opaque scene is resolved to a texture before water draws
  (also the source for refraction); water marches against the prepass depth and falls back to the
  prefiltered sky where the ray leaves the screen.

**12. HDR bloom + auto-exposure.** An energy-conserving bloom without threshold, and exposure that
adapts between interiors and open terrain, both before the frame grade. Night layers need it: with
the fixed exposure (−1.25 stops) The Falklands night terrain reaches the tonemap at about 0.0155, and
Khronos Neutral's toe (6.25·x² below 0.08) shows it at 5/255, where retail drew about 30/255. The
adaptation needs an upper limit so night stays dark (about retail's level), and the toe may need
softening for what stays below 0.08. Night particles (step 7) are checked here, on The Falklands night layer
(`gpm_cq 32`), once night exposure is settled.

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
- **Reference views:** saved camera views per map, visited unattended after autohost, each writing
  a screenshot, the pass timings and draw counts; plus benchmark paths. Lane `tools`, see
  [tools.md](tools.md).
- **Debug menu and overlay:** lane `ui`, see [ui.md](ui.md). Pass timings, renderer variables;
  debug views and spawning vehicles and kits come later.
