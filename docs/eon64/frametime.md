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

Muttrah City 2, `gpm_skirmish` 16, the same spot in the city, 4x MSAA, sun cascades on,
2026-11-18, engine `fd85ae94` (plugins with `/O2`):

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

## Plan

Parked on 2026-11-18: at 153 fps there is headroom, so the renderer roadmap goes first. Resume here.

1. Measure what the sun cascades cost the CPU in total (a run with them off) before redesigning
   the caster submission, which repeats the culling over the cascades' light volume.
2. Object culling (17 %): the object manager's grid and cull fade, the object drawer's box tests.
3. Per-draw cost: D3DX effect calls (15 %) and Ra parameter apply. Replacing D3DX's per-draw
   parameter and pass handling with the renderer's own constant uploads is `render` work: renderer
   step 9 ([renderer.md](renderer.md)), which also takes the sun caster culling of item 1.

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
