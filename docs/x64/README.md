# The `pr/x64-local` fork

An x64 Battlefield 2 client, built from OpenBF2, that runs the Project Reality mod (PR) offline
and replaces its renderer with a modern pipeline. The agent and contributor policy is
[AGENTS.md](../../AGENTS.md); this directory records the current state of the work.

## Where it comes from

```
B2BF/OpenBF2 main ─► kr4uzi/OpenBF2 main ─► + realitymod/OpenBF2 PR #1 ─► pr/x64-local
                                              (PR game-logic port)        (this fork)
```

Upstream is merged in, never rebased onto. The realitymod port arrived as one merge (`c4a3a850`)
and is treated as imported code.

## Lanes

Every commit belongs to one lane; the commit subject starts with it (`render(csm): …`).

| Lane | Contents | Document |
|---|---|---|
| `fix` | Bugs in the reconstructed engine | [upstream-fixes.md](upstream-fixes.md) |
| `compat` | What PR needs from the engine | [compat.md](compat.md) |
| `render` | The renderer uplift | [renderer.md](renderer.md) |
| `build` | Toolchain, CMake/Ninja, ABI checks | [building.md](../building.md), below |
| `dev` | Diagnostics and developer hooks | [flags.md](flags.md) |
| `upstream` | Imported code | — |

Commits before 2026-11-17 used `feat:`, `fix:`, `compat:`, `build:` prefixes and
`[CHOSEN, not recovered]` source tags. Those tags on this fork's own lines were replaced by lane
markers (`[FIX]`, `[COMPAT]`, `[RENDER]`, `[DEV]`, `[BUILD]`); upstream's tags are unchanged.

## Building

The client is built for x64 with MSVC, CPython 2.7 headers (`BF2_PYTHON27_SRC`) and Ninja.

- `BF2.exe` and the plugin DLLs (RendDX9, TextureAtlasBuilder, SwiffPlayer, BF2Audio) are
  incremental CMake targets. A one-file renderer change rebuilds in about 8 s; a no-op build takes
  about 5 s.
- Each plugin's sources are listed once, in `Code/Build/<plugin>_sources.txt`, and read by both
  the CMake target and the plugin's `.cmd` script.
- After each plugin link, `Code/Build/plugin_abicheck.cmake` checks the DLL's exports, imports and
  CRT against the shape the engine expects, and deletes the DLL if it drifted.
- `bf2_client_plugins` stages the DLLs next to `BF2.exe`.
- RendDX9's seam build (`BF2_GFX_SEAM`) still uses `renddx9.cmd`.

## Running

The client runs under Proton/DXVK against a PR install through an overlay, so the install itself
is never modified. Reversed-Z needs float depth; under DXVK set
`DXVK_CONFIG="d3d9.useD32forD24 = True"`. The x64 client also needs Microsoft's x64
`d3dx9_24.dll` and `d3dx9_25.dll` beside `BF2.exe` (see Issue A in
[upstream-fixes.md](upstream-fixes.md)).

Offline play is client-hosted (create local). `BF2_AUTOHOST=<map>:<mode>:<size>` hosts straight
from the menu.
