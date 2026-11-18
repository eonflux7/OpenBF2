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
| 8. Anti-aliasing | Not started | — |
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

**8. Anti-aliasing.** Fixed 4x MSAA as the one anti-aliasing method, with what MSAA alone misses
covered at its source. Now the prepass depth, normals and sun mask are single sample, so an edge pixel
reads one mask value for all its samples (halos on silhouettes, mostly foliage against the sky; on
bright terrain, dark lines along ridge silhouettes, where the edge reads the far slope's N.L and
shadow, seen on Kashan Desert). D3D9 cannot read individual samples of an MSAA surface, so a
screen-space term cannot be made per-sample; the mask goes instead.
- **Fixed 4x.** The scene target is always 4x, whatever the game's anti-aliasing setting; there is
  no 2x, 8x or off. 2x leaves alpha-to-coverage three levels (banded foliage); 8x doubles the
  memory and bandwidth of the float scene and its MRT for less than 0x to 4x gave; without MSAA,
  alpha-to-coverage and the LOD dither degrade to hard edges and raw dither. The sample count is one
  constant; only the coverage sharpening and the dither depend on it. The HUD's back buffer is
  single sample.
- **Prepass into the scene depth.** The prepass draws into the MSAA scene depth, and the opaque
  world pass tests EQUAL without writing depth, so each pixel shades about once. The StateManager
  forces the test for prepassed draws, overriding the effects' own `ZFUNC` / `ZWRITEENABLE`. EQUAL
  needs both passes to produce the same position bit for bit: each effect computes it in one shared
  function used by both techniques. Skinned meshes, terrain geomorphing and tree and overgrowth wind
  are the cases to check; a draw that cannot guarantee it is not prepassed. Draws that are not
  prepassed (undergrowth, first-person models, the sky) test and write depth as before; which queues
  are which is listed in [Frame](renderer-pipeline.md#frame) when the step lands. Linear depth is written as an MSAA
  target and resolved to a single-sample texture (s15). The resolve averages depth across a
  silhouette into a value no surface has, so soft particles fade against that average: the step 7
  edge fringe changes shape rather than going away.
- **Sun shadows in the lit shaders.** Every lit shader samples the cascades itself through one
  shared include (as water and undergrowth already do) and computes N.L and the cascades' weight
  against baked sun. The sun mask pass, its target and s14 are deleted, and with them the prepass's
  normals target (s12) until GTAO (step 13) needs one. Each triangle then shades its own edge
  samples. The cascade constants (`RASM`) are set once per frame as a fixed register range,
  not per effect through D3DX, which would add to the CPU-bound frame before step 9.
- **Centroid interpolation.** An edge pixel is shaded at its centre even when the centre lies outside
  the triangle, so interpolants are extrapolated: lightmap UVs read the neighbouring chart (seams
  along mesh edges) and the shadow lookup position leaves the surface (acne and leaks on
  silhouettes). Lightmap and atlas UVs and the world position used for the cascades are `_centroid`.
  Derivatives of centroid inputs are inexact, so mip selection, the coverage sharpening and the
  cascades' face normal take theirs from a non-centroid copy. The cascade position went in with the
  sun shadows sub-step (the owner's call, 2026-11-18), as each lit shader's own `SunPos` interpolant.
- **Alpha-to-coverage** for alpha-tested materials (foliage, fences, wires), in the prepass only:
  the main pass's EQUAL test already limits shading to the samples the prepass covered, so its alpha
  test goes and the two passes need not agree on alpha. Alpha is sharpened by its screen derivative
  to about a one-pixel edge, and corrected for the alpha the mips lose, so foliage does not thin out
  with distance. Retail enables it through vendor render-state hacks (NVIDIA `ATOC`); DXVK honours
  it (active only while a draw alpha-tests), so the whole mesh prepass runs inside retail's bracket.
- **Thin geometry.** MSAA cannot help what is thinner than its sample spacing; power lines, antennas
  and wires break into dashes. They are widened to at least about one pixel in the vertex shader and
  their alpha lowered by the same factor. They are recognised by material, or by flagging thin static
  meshes.
- **LOD crossfade.** Static, overgrowth and tree LOD switches and the distance cull fade become a
  dithered crossfade instead of popping: one screen-space Bayer threshold, `clip()` against it for
  the outgoing LOD and against its complement for the incoming one. Alpha-to-coverage cannot do
  this, because the masks it gives for a and 1 - a are not guaranteed complementary (holes and
  double cover).
- **Transparents and depth.** D3D9 binds a depth buffer only with a render target of the same
  sample count and cannot resolve depth. Water stays in the MSAA scene, drawn before the resolve, so
  its shoreline keeps its edge samples (and step 11's reflections and refraction read the resolved
  opaque scene plus water's own pass). After the resolve, a full-screen pass rebuilds a single-sample
  depth buffer from the resolved linear depth (`oDepth`), and the remaining transparents (particles,
  decals, effects) draw against it. Its edge depth is averaged, as for soft particles.
- **Sampler policy** (shader contract). Every material sampler (albedo, normal, detail, gloss) is
  16x anisotropic; data samplers state point or linear. The caps-dependent filter defines
  (`FILTER_*_MIN` / `_MAG`, `IS_NV4X`) are deleted.
- **Other views.** First-person models draw into the MSAA scene. Render-to-texture views (vehicle
  and guided-weapon cameras, scopes where they render the world) state whether they are MSAA or
  single sample. Tinnitus and thermals read the resolved linear scene; thermals keep their output.
- **Expected, not a bug.** Shading once per pixel leaves glossy surfaces sparkling; specular
  anti-aliasing is step 11.
- **Acceptance.** GPU timer spans for the 4x prepass, the resolve and the depth rebuild. Reference
  shots: foliage against the sky, a fence at 50 m and 200 m, Kashan Desert ridge lines, power lines,
  the edges of a lightmapped building (centroid), each compared with DXVK's
  `d3d9.forceSampleRateShading`, which turns 4x MSAA into 4x supersampling without engine changes:
  a reference to compare against, not a mode.
- **Order within the step**, each seen in game before the next: sampler policy; shadows in the lit
  shaders and the mask deleted, with the centroid cascade position; prepass into the MSAA depth with
  EQUAL, centroid UVs and the transparent depth rebuild; alpha-to-coverage; thin geometry; LOD
  crossfade.
- **Moved to step 10: the compressed resolve.** Averaging linear HDR leaves high-contrast edges
  aliased; writing colour compressed (Karis, 1 / (1 + luma)) before the resolve and expanding after
  fixes that. It cannot land before terrain is one pass: terrain's detail and point-light passes add
  (`ONE,ONE`) onto the scene, and a sum of compressed values is not the compressed sum.
- Not used: TAA and upscalers (motion vectors for skinned meshes, vehicles and particles; blur
  costs distant detail, which matters for spotting), post-process AA (edges only, unstable in
  motion), supersampling (about 4x the shading).

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
- **Reference views:** per-map lists of camera positions (with or without the first-person model,
  optionally with a spawned vehicle). After autohost the client visits each view, waits a fixed
  number of frames, and writes a screenshot and the pass timings, for regression and performance
  comparison between steps.
- **Debug menu and overlay:** lane `ui`, see [ui.md](ui.md). Pass timings, renderer variables;
  debug views and spawning vehicles and kits come later.
