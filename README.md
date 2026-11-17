# openbf2-eon64

An x64 Battlefield 2 client, built from [OpenBF2](https://github.com/kr4uzi/OpenBF2), that runs the
Project Reality mod (PR) offline and replaces BF2's renderer with a modern pipeline.

This is a work in progress. A successful build is not a playable game; the state of each area is
recorded in [`docs/eon64/`](docs/eon64/README.md).

## What it does

- **Runs PR 1.9 on x64.** PR's closed hook DLL (`rhooks.dll`) and its 32-bit Python extension cannot
  load into a 64-bit engine, so the engine provides what PR needs from them instead: CPython 2.7.12
  as `dice_py.dll`, PR's built-in Python modules, HUD variables, the launcher handshake. See
  [compat.md](docs/eon64/compat.md).
- **Replaces the renderer.** BF2's forward, gamma-space, lightmap-lit renderer becomes a linear HDR
  pipeline with reversed-Z, a depth prepass, cascaded sun shadows and, later, GTAO in place of
  lightmaps. See [renderer.md](docs/eon64/renderer.md).
- **Fixes engine bugs** found along the way (crashes, x64 hazards), kept separable so they can go
  upstream. See [upstream-fixes.md](docs/eon64/upstream-fixes.md).

### Renderer status

| Step | State |
|---|---|
| 1. Reversed-Z | Accepted |
| 2. Linear lighting + HDR scene | Accepted |
| 3. Shared lighting model | Accepted |
| 4. Depth prepass + screen-space sun mask | Accepted |
| 5. Cascaded sun shadows + PCF | In progress (`BF2_SUN_SHADOWS`) |
| 6. GTAO, then drop lightmaps | Not started |

## Lanes

Every change belongs to exactly one lane, and its commit subject starts with it
(`render(csm): blend between cascades`).

| Lane | What | Goes to |
|---|---|---|
| `fix` | Bugs in the reconstructed engine | Upstream |
| `compat` | What PR's content, scripts and launcher need from the engine | Possibly realitymod |
| `render` | The renderer uplift; new design, not retail | This fork |
| `build` | Toolchain, CMake/Ninja, ABI checks | Mostly upstream |
| `dev` | Diagnostics and developer hooks, off by default | Case by case |
| `upstream` | Imported code, not edited in place | — |

## Building

Windows x64 only, with MSVC (C++14), Ninja and CPython 2.7 headers (`BF2_PYTHON27_SRC`). x86 is not
built on this branch.

- `BF2.exe` and the four plugin DLLs (RendDX9, TextureAtlasBuilder, SwiffPlayer, BF2Audio) are
  incremental CMake targets; a one-file renderer change rebuilds in seconds.
- Each plugin link runs an ABI check (`Code/Build/plugin_abicheck.cmake`) that deletes the DLL if
  its exports, imports or CRT drifted.
- Plugin sources are listed once, in `Code/Build/<plugin>_sources.txt`, and read by both CMake and
  the `.cmd` scripts.

General toolchain notes are in [docs/building.md](docs/building.md) and
[docs/getting-started.md](docs/getting-started.md).

## Running

The client runs against an existing PR install through an overlay; the install itself is never
modified. Under Proton/DXVK:

- set `DXVK_CONFIG="d3d9.useD32forD24 = True"` (reversed-Z needs float depth);
- place Microsoft's x64 `d3dx9_24.dll` and `d3dx9_25.dll` beside `BF2.exe`.

Play is offline and client-hosted. `BF2_AUTOHOST=<map>:<mode>:<size>` hosts straight from the menu.
Every flag this fork adds is listed in [flags.md](docs/eon64/flags.md).

## What is not here

PR's shaders, scripts and Python shims are closed content and are not part of this repository. The
documents here describe only the engine side of each contract (console commands, event ids, sampler
slots, shader constants).

## Upstream

```
B2BF/OpenBF2 ─► kr4uzi/OpenBF2 ─► + realitymod/OpenBF2 PR #1 ─► openbf2-eon64
                                    (PR game-logic port)
```

OpenBF2 reconstructs the Battlefield 2 1.50 engine from the shipped binaries; this fork would not
exist without it. Upstream is merged in, never rebased onto. Upstream's own documentation remains
under [docs/](docs/README.md), and its fidelity-first policy applies when a `fix` or `build` change
is prepared for upstream.

## Contributing

Read [AGENTS.md](AGENTS.md) first: lanes, commit format, source markers, flags and what "done" means.
One lane per commit; upstreamable commits must not depend on `render` code. Agents do not open pull
requests or answer reviewers; the human author does and is responsible for the change.

## License

See [LICENSE](LICENSE).
