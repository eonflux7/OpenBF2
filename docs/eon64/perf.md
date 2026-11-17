# Load times

The `perf` lane ([AGENTS.md](../../AGENTS.md)): make loading faster without changing what is
loaded. The target is the **first map load**, menu to spawn screen, with PR.

## Measuring

`BF2_LOAD_TRACE=<prefix>` ([flags.md](flags.md)) records timed spans on the main thread (and later
on worker threads). Each image writes its own file, and both share one clock:

| Category | Site | What it covers |
|---|---|---|
| `phase` | `BF2Engine::init`, `initEngine`, `startGame`, `GameLogic::load` | `boot`, `engine.init`, `game.start`, `level.load` |
| `mark` | `GameLogic::load`, `LoadStat::setNextLoadPart`, first `renderMenu` | `load:*` sections, `part:*` loading-bar phases, `menu.first` |
| `mount` | `FileManager::mountArchive` | Opening an archive and reading its directory |
| `open.arc` / `open.loose` / `open.miss` | `FileManager::open` (read) | Where the file resolved; archive entries up to 64 KiB are inflated here |
| `exists.arc` / `exists.loose` / `exists.miss` | `FileManager::fileExistsEx` | The same for existence probes |
| `con` | `OldConsole::run` | One script, with nested scripts as children |
| `tex` | `TextureManager::loadTextureFromFile` | Read, decode, create |
| `mesh` | `MeshTemplate::loadFromStream` | Static, bundled and skinned mesh files |
| `fx.src` | `compileEffectFile` (`RendDX9/Effect.cpp`) | Effects compiled from `.fx` source with no cache, on every load |
| `fx.dice` | `DiceEffect::compile` | Dice effects; `fx.compile` inside it when its cache missed |
| `fx.ra` | `EffectManager::compileEffect` | Ra effects; inside it `fx.key` (preprocessing for the content key), `fx.cacheload`, `fx.compile` (one per shader profile attempted), `fx.compile.fail`, `fx.create` |
| `py.init` / `py.admin` / `py.event` | `PythonHost` | Interpreter start, admin module, event handlers of 1 ms or more |
| `frame` | `BF2Engine::mainLoop` | Frames over 50 ms; after a load, these show work deferred to first use |

`Code/Tools/LoadTrace/loadtrace.py <prefix>` nests the spans and prints, per phase, the self time
of each category, the time between marks, and the heaviest names. "(untraced)" is phase time
that no instrumented span covers.

## Baseline

First load of muttrah_city_2 (PR, autohost, Wine), effect cache not invalidated, before any
`perf` change. From the menu click to the end of loading: about 190 s.

| Where | Time | What |
|---|---|---|
| Ra effect compiles | 176 s | 1338 effects, all compiled from source: the `.cfx` cache was never used under Wine (below). About 37 s are terrain effects during `Init.con`/`Sky.con` (0.6 to 0.76 s each), 133 s are mesh effects in the loading bar's Geometries part |
| Console scripts | 9 s | 13,305 runs |
| File opens | 1.5 s | 40,000 opens, mostly from archives |
| Startup to menu | 10.4 s | 5.2 s HUD scripts, 2 s `compileEffectFile` (`fsquaddrawer.fx` alone 1 s) |

The Ra cache key is computed with `D3DXPreprocessShader` from `d3dx9_24.dll`, which does not
export it (neither Microsoft's x64 build the game loads nor Wine's), so every key was empty and
every `.cfx.key` held `-`.
The key now falls back to `d3dcompiler_47`'s `D3DPreprocess`. With the cache filled, the same load
takes 37 s instead of 186 s (to the loading bar's Textures part): Ra effects 19.5 s, of which 16.6 s
is computing the content key (about 12 ms per effect) and 0.5 s reading the 1337 blobs. Console
scripts (9.4 s, all in the Objects part) are now the largest item.

### Recompiling every effect

With every Ra effect stale (after a shader edit, or a new cache), the same load reached the
Textures part in 186 s with the serial compile and in 31 s with the prefetch. 8 workers, each with
its own copy of the compiler module, ran 202 s of compiles in 25 s of wall time, mostly before the
main thread asked for the effects.

D3DX's effect compiler runs one compile at a time per loaded module: 8 compiles of a test effect
took 0.75 s serially, 0.77 s on 8 threads sharing `d3dx9_24.dll`, and 0.16 s on 8 threads with a
renamed copy each. The game runs with Microsoft's x64 `d3dx9_24`/`d3dx9_25`, as Wine's builtin
D3DX cannot compile `.fx`; that `d3dx9_24` has no `D3DXPreprocessShader` either.

## What the code already shows

These come from reading the code. The baseline trace will confirm or rule out each one.

- Effects take three paths. `compileEffectFile` and `DiceEffect` build BF2's mesh and terrain
  effects from source. The Ra path (`EffectManager`) has the per-mod `.cfx` cache.
  `compileEffectFile` has no cache, so it compiles from source on every load.
- A Ra cache hit still preprocesses the full effect source to compute its content key (`fx.key`).
- On a Ra cache miss, the compile tries the pixel shader profiles in order, starting at `ps_1_3`
  unless `GSQuickShaderPermutation` is set. Each profile that fails costs a full compile.
- The D3D9 device is created without `D3DCREATE_MULTITHREADED`. Worker threads may compile
  bytecode and decode files, but every device call stays on the main thread.
- `FileManager` resolves a name by asking each archive mounted for its directory, then probing
  loose files along every search path. A miss therefore costs one file system probe per path
  entry, and those probes are slow under Wine.
- Console scripts, template creation and Python run in order and change global state. They stay
  serial; only the file work under them can move to other threads.

## Plan

1. Baseline: `BF2_LOAD_TRACE` on a first PR map load, cold and warm effect cache.
2. Serial fixes the trace points to: file resolution, repeated work.
3. A job system: a fixed worker pool, and a commit queue that only the main thread drains.
4. Effects: compile bytecode on workers (device-free D3DX calls), create effects on the main
   thread. Cache the source-compiled paths.
5. Files: read and inflate ahead of the scripts. Decode textures and meshes on workers, create
   the D3D resources on the main thread.

## State

| Step | State |
|---|---|
| Load trace (`dev`) | Done; baseline above |
| Ra cache key under Wine (`fix`) | Done: seen in game, muttrah_city_2, 186 s to 37 s with a filled cache |
| Ra cache key from source and includes, no preprocessor (`BF2_PERF_FXKEY`) | Seen in game: keys 16.6 s to 2.2 s per load; awaiting acceptance |
| Ra effect prefetch: stale effects listed in `effects.lst` compile on worker threads (`BF2_PERF_FXPREFETCH`) | Seen in game: a full recompile loads in 31 s instead of 186 s; awaiting acceptance |
