# Load times

The `perf` lane ([AGENTS.md](../../AGENTS.md)): make loading faster without changing what is
loaded. The target is the **first map load**, menu to spawn screen, with PR.

## Measuring

`BF2_LOAD_TRACE=<prefix>` ([flags.md](flags.md)) records timed spans on the main thread (and later
on worker threads). Each image (BF2.exe, RendDX9, BF2Audio) writes its own file, and all share one clock:

| Category | Site | What it covers |
|---|---|---|
| `phase` | `BF2Engine::init`, `initEngine`, `startGame`, `GameLogic::load` | `boot`, `engine.init`, `game.start`, `level.load` |
| `mark` | `GameLogic::load`, `LoadStat::setNextLoadPart`, first `renderMenu`, autohost | `load:*` sections, `part:*` loading-bar phases, `menu.first`, `menu.host` (where a player's click would be) |
| `start` / `setup` | `BF2Engine::startGame`, `GameLogic::load` | Hosting: `game.init`, `gameLogic.init`, `startServer`; level setup: `beginLoadStats`, `createSunTemplate`, `loadingTemplatesEvent` |
| `init` | `BF2Engine::init`, `initEngine` | Startup stages: `modules` (inside it `modules.construct`, `modules.register`, `modules.singletons` and one span per module init, named by class), `filesystem`, `archives`, `localization`, `usersettings`, `modinfo`, `renderer` (inside it `renderer.window`, `renderer.dll`, `renderer.initialize`), `input`, `hud`, `swiff`, `network` |
| `mount` | `FileManager::mountArchive` | Opening an archive and reading its directory |
| `open.arc` / `open.loose` / `open.miss` | `FileManager::open` (read) | Where the file resolved; archive entries up to 64 KiB are inflated here |
| `exists.arc` / `exists.loose` / `exists.miss` | `FileManager::fileExistsEx` | The same for existence probes |
| `con` | `OldConsole::run` | One script, with nested scripts as children; inside it `con.read` (the file read) and `con.cmd` (one per invoked binding, named `Object.method`). The `con` self time is parsing and dispatch; each trace mark also prints `[CONPROFILE]` to stderr, the interpreter's time per stage since the previous mark (`split`, `compiler`, `firstToken`, `prep`, `keyword`, `exchange`, `lookup`, `args`, `checks`, `post`) |
| `tex` | `TextureManager::loadTextureFromFile` | One texture; inside it `tex.find` (path lookup), `tex.open` (open and header), `tex.create` (the D3D texture), `tex.read` (one per mip read into the locked level, inflate included) |
| `mesh` | `MeshTemplate::loadFromStream` | Static, bundled and skinned mesh files; inside it `mesh.read` (with `mesh.geometry`, the vertex and index blocks), `mesh.merge`, `mesh.buffers` (vertex and index buffers), `mesh.finalize`. `mesh.instances` is the template's walk over its live instances after the load |
| `snd.decode` / `snd.upload` | `SoundFile::load` (BF2Audio) | Reading and decoding one sound file; its OpenAL upload. `snd.unload` and `snd.sort` are the sweeps around the loads |
| `fx.src` | `compileEffectFile` (`RendDX9/Effect.cpp`) | Effects compiled from `.fx` source; inside it `fx.src.cache` (the bytecode cache lookup) and `fx.src.compile` |
| `fx.dice` | `DiceEffect::compile` | Dice effects; `fx.compile` inside it when its cache missed |
| `fx.ra` | `EffectManager::compileEffect` | Ra effects; inside it `fx.read` (the source), `fx.stat` (each file-info lookup), `fx.key` (the content key), `fx.keyfile` (the stored key), `fx.setup` (before the source read), `fx.listen`, `fx.event` (the loading-progress event), `fx.cacheload`, `fx.compile` (one per shader profile attempted), `fx.compile.fail`, `fx.create` |
| `py.init` / `py.admin` / `py.gamemode` / `py.event` | `PythonHost` | Interpreter start, admin module, the game-mode import and init, event handlers of 1 ms or more |
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
| Ra cache key from source and includes, no preprocessor | Done: accepted in game; keys 16.6 s to 2.2 s per load |
| Ra effect prefetch: stale effects listed in `effects.lst` compile on worker threads | Done: accepted in game; a full recompile loads in 31 s instead of 186 s |
| Console lines split and tokenized without `std::istringstream`, reproducing its token, `std::ws`, `get` and `getline` behaviour and state bits (`BF2_PERF_CONPARSE`) | In development: the interpreter's own time was 7.3 s of part:Objects over 2.6 million lines; the four streams per line were about 4.5 s of it |
| Id and name index for ClassManager's class list (`BF2_PERF_CLASSINDEX`) | In development: module registration took 1.0 s of startup (two scans per class registered) |
| Name index for the HUD's BFMeme registry, beside each container's child list (`BF2_PERF_MEMEINDEX`) | In development: HUD setup took 5.3 s of startup, each hudBuilder command slower than the last (9 us in the first tenth, about 300 us in the last) |
| Loading-screen repaints from effect loads at most thirty per second, as the load callback's (`BF2_PERF_FXEVENT`) | In development: 1,116 repaints in the geometry part, 1.15 s |
| Ra effect key: each include file's info looked up once per frame (`BF2_PERF_FXSTAT`) | In development: 30,051 lookups at the menu switch (2.3 s) and 24,552 in the geometry part (1.9 s) |
| Bytecode cache for `compileEffectFile` effects, `@HOME@/<mod>/cache/fxsrc/`, keyed by source, defines, flags and every include read (`BF2_PERF_FXSRC`) | In development; traced in game on muttrah_city_2: the second start compiles none (boot to menu 10.6 s to 8.75 s). These took 2 s before the menu, 3.9 s at game start (the define-change reload compiles them again) and 0.7 s in the level load |
