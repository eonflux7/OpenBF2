# Frame time

The `perf` lane ([AGENTS.md](../../AGENTS.md)) for the running game: make frames cheaper without
changing what is drawn. Load times are in [perf.md](perf.md).

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
- **Draws.** `BF2_DRAW_STATS` ([flags.md](flags.md)) for what each pass issues.

## Baseline

Muttrah City 2, `gpm_skirmish` 16, standing in the city, 4x MSAA, sun cascades on, 2026-11-18
(engine `074b3d39`):

- **13.3 ms a frame (76 fps)**, `game.lockFps` 200. The main thread is at 100 % of one core, DXVK's
  submission thread at 12 %: the frame is CPU-bound.
- GPU: about 5.3 ms of pass work (sun cascades 2.1, depth prepass 1.3, world opaque 1.3, sun mask
  0.2, the rest under 0.3); the remainder is idle.

Main thread, 10 s of samples:

| Share | What |
|---|---|
| 21 % | Outside the engine images: DXVK's `d3d9.dll` 6.7 %, C runtime 3.5 %, Wine, Flash, Python |
| 16 % | Mesh draws: Ra shader parameter apply, `StaticMesh::drawVanilla`, stream binding |
| 15 % | Object culling: frustum and box tests, culler walks, cull fade |
| 10 % | Sun caster pass: capture, sort, per-cascade test, draw |
| 6 % | `Renderer::tailField`: a linear scan of the Renderer's field table, per object |
| 5 % | State manager: constant uploads, pre-draw apply |
| 4 % | Render queue walks and sorts |
| 22 % | Spread over the rest of RendDX9 and BF2.exe |

## Plan

1. `Renderer::tailField` answers by offset from a table built once, instead of scanning (`perf`).
2. The sun casters are sorted by a key computed once per caster, instead of following pointers in
   every comparison (`render`).
3. Measure what the sun cascades cost the CPU in total (a run with them off) before redesigning
   the caster submission, which repeats the culling over the cascades' light volume.
4. Then the larger costs: per-draw parameter apply and culling.

## State

- `Renderer::tailField` answers from a table indexed by retail offset (plan 1). Built; not yet
  measured in game.
