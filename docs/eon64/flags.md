# Flags

Every environment variable this fork adds. Upstream's own `BF2_*` variables (traces, probes, test
harness switches) are not listed. Rules: [AGENTS.md](../../AGENTS.md#feature-flags).

## `render`: exist only while a step is in development

| Flag | Default | Effect | Removed when |
|---|---|---|---|
| `BF2_SUN_SHADOWS=1` | off | Sun cascaded shadow maps feed the sun mask | Step 5 is accepted in game; the cascades then always run and legacy shadow maps are deleted |
| `BF2_CSM_DISTANCE` | 400 | View depth (m) the cascades cover | Step 5 tuning fixes the value |
| `BF2_CSM_RES` | 2048 | Resolution of each cascade | Step 5 tuning fixes the value |
| `BF2_CSM_CASTER_REACH` | 500 | How far (m) toward the sun beyond a cascade casters still draw | Step 5 tuning fixes the value |

Retired with their steps on 2026-11-17 (`2b3fec82`): `BF2_REVERSED_Z`, `BF2_HDR`,
`BF2_FRAME_GRADE`, `BF2_DEPTH_PREPASS`. Their behaviour is now unconditional.

## `perf`: select the old serial path while a step is in development

| Flag | Default | Effect | Removed when |
|---|---|---|---|
| `BF2_PERF_CLASSINDEX=0` | on | ClassManager finds classes by id and name by scanning its class list again, with no index | The class index is accepted in game |
| `BF2_PERF_MEMEINDEX=0` | on | The HUD's name registry (BFMeme) walks its child lists for every lookup and insert again, with no name index | The registry index is accepted in game |
| `BF2_PERF_FXEVENT=0` | on | Every effect load posts a loading-screen repaint again, instead of at most thirty per second | The repaint throttle is accepted in game |
| `BF2_PERF_FXSTAT=0` | on | The Ra effect key looks up the size and write time of every include file for every effect again, instead of once per frame | The file-info memo is accepted in game |
| `BF2_PERF_FXSRC=0` | on | Effects loaded through `compileEffectFile` (fsquaddrawer, postproduction, skydome, terrainshader, ...) compile from source on every load again, with no bytecode cache | The source-effect cache is accepted in game |

Retired with their steps on 2026-11-17: `BF2_PERF_FXKEY`, `BF2_PERF_FXPREFETCH`.

## `compat`: gate behaviour retail BF2 must not see

| Flag | Effect |
|---|---|
| `BF2_HUD_AUTOVAR=1` | Unknown HUD variables are registered on first use; the rhooks HUD event channel (game events 9–12 with mod subtypes) is logged once and dropped instead of asserting |
| `BF2_CMDLINE_FILE=<path>` | The menu reads and writes its command-line file at this path under the install root (PR's launcher handshake) |

## `dev`: permanent, off by default, inert when unset

| Flag | Effect |
|---|---|
| `BF2_AUTOHOST=<level>:<mode>:<size>` | Once a profile is logged in, hosts a local game as Create local → Deploy would (e.g. `muttrah_city_2:gpm_skirmish:16`) |
| `BF2_FILE_MONITOR=1` | Turns on retail's GSFileChangeMonitor: changed loose shaders and textures reload while the game runs |
| `BF2_DEVCMD_FILE=<path>` | When the file changes, each line except `rem` runs on the console; output goes to stderr as `[DEVCMD]` |
| `BF2_CONSOLE_TRACE=1` | Logs console script runs and every `if`/`elseIf` result |
| `BF2_LOAD_TRACE=<prefix>` | Writes timed spans of load work (phases, file opens, scripts, textures, meshes, effect compiles, Python, frames over 50 ms) to `<prefix>-bf2.tsv`, `<prefix>-renddx9.tsv` and `<prefix>-bf2audio.tsv`; `=1` uses the prefix `loadtrace`. Summarize with `Code/Tools/LoadTrace/loadtrace.py <prefix>`. See [perf.md](perf.md) |
| `BF2_AI_WEAPON_TRACE` | Logs each AI weapon record released when its `WeaponObject` is destroyed |

## Build options (CMake)

| Option | Effect |
|---|---|
| `BF2_PYTHON27_SRC=<dir>` | Compiles the engine against these CPython 2.7 headers and puts `pylib-2.7.12.zip` on `sys.path`; `dice_py.dll` itself is built by `Code/Build/python27/` |
| `BF2_GFX_SEAM` | Builds RendDX9 through `renddx9.cmd` (the seam build) instead of the Ninja target |

## Runtime environment (not ours, but required)

| Variable | Why |
|---|---|
| `DXVK_CONFIG="d3d9.useD32forD24 = True"` | DXVK refuses `D24FS8`; this gives reversed depth a float depth buffer |
| `WINEDLLOVERRIDES=d3dx9_24,d3dx9_25=n` | Wine's builtin D3DX stubs break effect compilation (see upstream-fixes Issues A and C) |
