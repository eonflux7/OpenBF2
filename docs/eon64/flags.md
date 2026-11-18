# Flags

Every environment variable this fork adds. Upstream's own `BF2_*` variables (traces, probes, test
harness switches) are not listed. Rules: [AGENTS.md](../../AGENTS.md#feature-flags).

## `render`: exist only while a step is in development

None at present. Retired with step 5 on 2026-11-18: `BF2_SUN_SHADOWS`, `BF2_CSM_DISTANCE`,
`BF2_CSM_RES`, `BF2_CSM_CASTER_REACH`.

Retired with their steps on 2026-11-17 (`2b3fec82`): `BF2_REVERSED_Z`, `BF2_HDR`,
`BF2_FRAME_GRADE`, `BF2_DEPTH_PREPASS`. Their behaviour is now unconditional.

## `perf`: select the old serial path while a step is in development

None at present. Retired with their steps on 2026-11-17: `BF2_PERF_FXKEY`, `BF2_PERF_FXPREFETCH`,
`BF2_PERF_CONPARSE`, `BF2_PERF_CLASSINDEX`, `BF2_PERF_MEMEINDEX`, `BF2_PERF_FXEVENT`,
`BF2_PERF_TSCCAL`, `BF2_PERF_VIDEODETECT`, `BF2_PERF_FILEPREFETCH`, `BF2_PERF_CONDISPATCH`,
`BF2_PERF_SWIFFIMG`, `BF2_PERF_PROBECACHE`, `BF2_PERF_FXSTAT`, `BF2_PERF_FXSRC`. Their behaviour is
now unconditional.

## `compat`: gate behaviour retail BF2 must not see

| Flag | Effect |
|---|---|
| `BF2_HUD_AUTOVAR=1` | Unknown HUD variables are registered on first use; the rhooks HUD event channel (game events 9–12 with mod subtypes) is logged once and dropped instead of asserting |
| `BF2_LEVEL_INIT_ARGS=1` | The level `Init.con` gets `v_arg1` = `host`/`client`, `v_arg7` = game mode, `v_arg8` = layer (PR's layer-specific init); retail passes all empty |
| `BF2_CMDLINE_FILE=<path>` | The menu reads and writes its command-line file at this path under the install root (PR's launcher handshake) |

## `dev`: permanent, off by default, inert when unset

| Flag | Effect |
|---|---|
| `BF2_AUTOHOST=<level>:<mode>:<size>` | Once a profile is logged in, hosts a local game as Create local → Deploy would (e.g. `muttrah_city_2:gpm_skirmish:16`) |
| `BF2_FILE_MONITOR=1` | Turns on retail's GSFileChangeMonitor: changed loose shaders and textures reload while the game runs |
| `BF2_DEVCMD_FILE=<path>` | When the file changes, each line except `rem` runs on the console; output goes to stderr as `[DEVCMD]` |
| `BF2_CONSOLE_TRACE=1` | Logs console script runs and every `if`/`elseIf` result |
| `BF2_LOAD_TRACE=<prefix>` | Writes timed spans of load work (phases, file opens, scripts, textures, meshes, effect compiles, Python, frames over 50 ms) to `<prefix>-bf2.tsv`, `<prefix>-renddx9.tsv` and `<prefix>-bf2audio.tsv`; `=1` uses the prefix `loadtrace`. Summarize with `Code/Tools/LoadTrace/loadtrace.py <prefix>`. See [perf.md](perf.md) |
| `BF2_DRAW_STATS=<path>` | Counts draws per effect, technique and pass; once a second rewrites `<path>` (TSV) with session totals and the last presented frame. Shows which effects actually draw. Sun caster draws are split per cascade (technique `RaShadowDepth@c0`..`@c3`), and counter rows (`(count)`) give the captured and unbounded casters per frame. See [renderer.md](renderer.md) step 6 |
| `BF2_PY_ERRORS=1` | Prints a script event handler's Python exception to stderr as `[PYERR]` lines (traceback module format) before clearing it; unset, it is cleared silently as retail does |
| `BF2_GPU_TIMERS=<path>` | GPU time per renderer pass from D3D9 timestamp queries, read a few frames late so it never waits on the GPU. Once a second rewrites `<path>` (TSV): per span, the mean and maximum over the last second and the session mean, in ms, plus `(frame)` for the whole frame. Spans run from one mark to the next, so they include GPU idle time when the CPU is the bottleneck. The performance overlay (`renderer.uiOverlay`, [ui.md](ui.md)) runs the same queries while it is shown, without the file. See [renderer.md](renderer.md#tooling) |
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
