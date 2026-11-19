# Frame time

The `perf` lane ([AGENTS.md](../../AGENTS.md)) for the running game: make frames cheaper without
changing what is drawn. Load times are in [perf-load.md](perf-load.md).

Work in renderer code that is this fork's own (the sun cascades, the depth prepass) is `render`
work even when its purpose is speed; this document records its cost alongside, so the frame is
seen as a whole.

## Measuring

- **Who is the bottleneck.** Per-thread CPU use of the game (`top -H` on the BF2.exe process): a
  main thread at 100 % while the GPU spans leave idle time means the frame is CPU-bound. Check
  `game.lockFps` first; a frame cap looks the same.
- **GPU per pass.** `BF2_GPU_TIMERS` ([flags.md](flags.md)). Spans run from mark to mark, so when
  the CPU is the bottleneck the GPU's idle time lands in the span that waits (usually `hud`, which
  runs to Present).
- **CPU per function.** Sample the main thread with Linux `perf record -t <tid>` while the game
  runs under Wine. perf cannot name the PE images, so the sampled addresses are resolved by hand:
  module bases from `/proc/<pid>/maps` (each image's header page is mapped from its file), sizes
  from the PE headers, symbols from the link maps (`BF2.map`, `RendDX9.map`). A link map lists public
  symbols only, so samples in static or inlined code are credited to the nearest public symbol
  before them: object-file totals are reliable, single function names approximate.
- **Draws.** `BF2_DRAW_STATS` ([flags.md](flags.md)) for what each pass issues. It costs about
  9 % of the main thread itself (its lookups), so CPU samples are taken in a session without it.
- **Sun cascades' whole cost.** `renderer.sunShadows 0` at the same view: the frame-time difference
  is the caster pass, CPU and GPU together.

## Baseline

Before render milestone 8. Muttrah City 2, `gpm_skirmish` 16, the same spot in the city, 4x MSAA,
sun cascades on, 2026-11-18, engine `fd85ae94` (plugins with `/O2`):

- **6.6 ms a frame (153 fps)**, `game.lockFps` 200. The main thread is still at 100 %, DXVK's
  submission thread at 23 %: still CPU-bound.
- GPU: about 3.5 ms of pass work (sun cascades 1.4, depth prepass 0.8, world opaque 0.9, sun mask
  0.1).

Main thread by image, 10 s of samples:

| Share | Image |
|---|---|
| 46 % | RendDX9.dll |
| 18 % | BF2.exe: object manager culling and cull fade, game logic |
| 15 % | d3dx9_25.dll: the D3DX effect framework, called per draw (parameters, passes, commits) |
| 13 % | d3d9.dll (DXVK) |
| 6 % | C runtime |
| < 1 % each | BF2Audio, Python, SwiffPlayer |

Main thread by subsystem (RendDX9 and BF2.exe): object culling 17 %, mesh draws and parameter apply
14 %, sun caster pass 7 %, state manager 5 %, render queues 3 %, `tailField` 0.8 %.

## After render milestone 8

Milestone 8's LOD policy (full-detail terrain, LOD 0 bundled meshes, static switch distances x3, no
distance cull) adds draws: the owner sees about 20 % lower frame rate on Muttrah City. Still
CPU-bound: the GPU spans sum to about 5.7 of 12.6 ms on Muttrah City and 4.9 of 8.25 ms on
Saaremaa; the rest is the `hud` span, the wait in Present.

Saaremaa, overview camera over forest and buildings, 1600x900, 2026-11-19: depth prepass 1.47 ms,
world opaque 1.22, sun cascades 1.53, resolve 0.19, depth rebuild 0.05.

GPU-side lead: `Ra_LodDitherClip` runs in every lit shader, so opaque shaders lose early depth
rejection even at dither (0, 1); a static branch or a separate dithered technique would avoid it.

## Render milestone 9, task 1

Fixed views, 1600x900, 4x MSAA, vsync off, `game.lockFps 1000`, engine `aa754216`, 2026-11-19.
Muttrah City 2 `gpm_skirmish` 16, view `high` (overview over the city); Fallujah West `gpm_cq` 64,
view `city1`. Main thread at 100 % and the `hud` span (the wait in Present) large in every case:
CPU-bound. Times are per frame; CPU shares of the main thread are turned into ms of the frame.

| | Muttrah `high` | Fallujah `city1` |
|---|---|---|
| Frame, cascades on | 14.9 ms | 28.9 ms |
| Frame, `renderer.sunShadows 0` | 8.7 ms | 15.6 ms |
| Caster pass, whole cost | 6.2 ms (42 %) | 13.3 ms (46 %) |
| Draws: cascade c0 / c1 / c2 / c3 | 0 / 97 / 1232 / 3939 | 31 / 228 / 2437 / 7181 |
| Draws: depth prepass | 2507 | 4524 |
| Draws: lit and other passes | 2770 | 4623 |
| Draws, total | 10545 | 19024 |

Draws are the last frame of a `BF2_DRAW_STATS` session at the same view. Nearly all of them are
static mesh materials, one draw per material in every pass (the caster and prepass techniques
included); terrain, overgrowth and roads are a few hundred. Cascade c3 (about 107 to 400 m) holds
three quarters of the caster draws at about 185 triangles a draw; on Fallujah the fog hides most
of what it draws.

Main thread by image and object file, ms per frame, cascades on / off / difference:

| | Muttrah on | off | diff | Fallujah on | off | diff |
|---|---|---|---|---|---|---|
| RendDX9.dll | 8.06 | 4.27 | 3.79 | 15.62 | 8.04 | 7.58 |
| d3dx9_25.dll (effect framework) | 2.44 | 1.46 | 0.98 | 5.32 | 2.58 | 2.75 |
| d3d9.dll (DXVK) | 2.05 | 1.41 | 0.64 | 3.62 | 2.42 | 1.20 |
| BF2.exe | 1.29 | 0.90 | 0.39 | 2.60 | 1.51 | 1.09 |
| StateManager | 1.83 | 1.00 | 0.83 | 3.30 | 1.72 | 1.58 |
| RenderQueues | 1.58 | 1.04 | 0.54 | 2.69 | 1.76 | 0.93 |
| RaShader | 1.22 | 0.63 | 0.59 | 2.49 | 1.37 | 1.11 |
| StaticMesh | 1.07 | 0.48 | 0.59 | 2.40 | 1.03 | 1.37 |
| ObjectDrawer | 0.77 | 0.23 | 0.54 | 2.08 | 0.64 | 1.44 |

Top functions (nearest public symbol, Muttrah, cascades on): StateManager's pre-draw apply 8.4 %,
`RaShader::applyParameterVector` 5.6 %, `StaticMesh::drawVanilla` 4.1 %, render queue walk 2.5 %,
`renderQueuesDrawShadowCasters` 2.4 %, `StaticMeshMaterial::applyStreams` 2.3 %, box-frustum tests
1.5 %, `ObjectDrawer::submitShadowCasters` 1.3 %.

Read:
- The caster pass is the largest single cost, and most of it is per draw: D3DX, the state manager,
  Ra parameters, mesh draws and DXVK. Its own culling (ObjectDrawer, the BF2.exe cullers, the caster
  sort) is about 1 ms on Muttrah and 2.5 ms on Fallujah.
- The main thread spends about 1.4 µs (Muttrah) to 1.5 µs (Fallujah) per draw, culling included.
- Leads for the task order: draws in the depth-only passes (prepass, casters) could be one per mesh
  rather than one per material where the materials share the depth shader (no alpha test); and
  contribution culling applied to casters by their size in cascade texels would remove much of
  cascade c3's small draws.

## Render milestone 9, task 5

Main thread at Muttrah `high`, 10 s samples, engine with task 5, 2026-11-19. Shares of the main
thread, programs (`renderer.submit 1`) / D3DX (`0`), both with the banded depth order:

| | Programs | D3DX |
|---|---|---|
| d3dx9_25.dll | 5.5 % | 15.7 % |
| RaShader.cpp | 2.3 % | 6.8 % |
| Submit.cpp | 12.6 % | — |
| StateManager.cpp (pre-draw apply 10.1 / 9.7) | 14.0 % | 12.8 % |
| d3d9.dll (DXVK) | 12.8 % | 13.0 % |

Submit.cpp's largest parts: the holder calls into the sink (`applyUsed` 3.8 %), matrix and vector
conversion (`setMatrices` 2.5, `setVectors` 1.6), register writes (`emitTarget` 1.6). The first
version (strict near-to-far order, a conversion per component) spent 16.9 %: the order switched
program at almost every prepass draw, each switch binding the program's states, defaults and global
parameters again. `bench1`: 12.44 ms programs, 12.92 ms D3DX; before the banded order 13.46 / 13.52.

Then float values copied whole into the sink and register-shaped targets (row-packed matrices,
vectors) uploaded in place, without the per-component conversion and gather. Same-session pairs,
3 runs each, skirmish layer:

| Path | Programs | D3DX | Gain |
|---|---|---|---|
| Muttrah City `bench1` | 12.03 ms | 13.07 ms | 8.0 % |
| Fallujah West `bench` | 21.52 ms | 23.43 ms | 8.2 % |

Fallujah West has the most draws (16085 a frame), so the per-draw work shows most there; Muttrah
City's runs spread less (about 2 % against 3 %), so it separates small changes better.

Then the pre-draw apply: two thirds of its samples were the pixel constant flush scanning one dirty
byte per register from the material constants (c0 up) to the LOD dither (c222), about 210 clean
bytes a draw. With the dirty registers as a bit mask (`f3515fb4`) the pre-draw apply and the flush
take 2.5 % of the main thread, from 8.2 % (Muttrah `high`, programs, 10 s samples). Both paths
go through the state manager, so both gain. Same-session pairs, 3 runs each, skirmish layer:

| Path | Programs | D3DX | Gain |
|---|---|---|---|
| Muttrah City `bench1` | 10.86 ms (12.03 before) | 11.73 ms (13.07) | 7.4 % |
| Fallujah West `bench` | 19.40 ms (21.52) | 20.78 ms (23.43) | 6.6 % |

## Plan

Parked on 2026-11-18, when 153 fps left headroom and the renderer roadmap went first. Resume here
after render milestone 9, which takes items 1 and 3.

1. Measure what the sun cascades cost the CPU in total (a run with them off) before redesigning
   the caster submission, which repeats the culling over the cascades' light volume.
2. Object culling (17 %): the object manager's grid and cull fade, the object drawer's box tests.
3. Per-draw cost: D3DX effect calls (15 %) and Ra parameter apply. Replacing D3DX's per-draw
   parameter and pass handling with the renderer's own constant uploads is `render` work: render
   milestone 9 ([renderer.md](renderer.md#9-draw-submission)), which also takes the sun caster
   culling of item 1.

Open:
- An intermittent stack overflow while loading, seen once with the unoptimised plugins
  (`virtual_setup_exception stack overflow`, address at RendDX9 offset `0x821d` of engine `b3342099`).
  Rebuild that commit's `RendDX9.map` to name the function; with `/O2` it may have moved or gone. If
  it returns, `WINEDEBUG=+seh` with the module base pins it.

## State

Seen in game on Muttrah:
- The plugin DLLs compile with `/O2` (`BF2_PLUGIN_OPTIMIZE`): 12.2 ms to 6.6 ms a frame
  (13.3 ms at the baseline before it); RendDX9, SwiffPlayer and BF2Audio had been built without optimisation.
- `Renderer::tailField` answers from a table indexed by retail offset: 5.6 % to 0.8 % of the main
  thread (measured before `/O2`).
- The sun casters carry their sort keys (shader, material), copied at capture. No measurable gain:
  the sort's cost had been the unoptimised insertion sort, not the pointer chasing.
