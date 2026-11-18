# openbf2-eon64 documents

The current state of the work. The rules are in [AGENTS.md](../../AGENTS.md); building and running
are in the [README](../../README.md). Start with [status.md](status.md).

| Document | What it records |
|---|---|
| [status.md](status.md) | Where each lane stands; the current milestone, its tasks and open issues |
| [renderer.md](renderer.md) | `render` plan: the roadmap and the milestones still to do |
| [renderer-pipeline.md](renderer-pipeline.md) | `render` as built: accepted milestones, the frame, GPU pass timers, sun shadows, the shader contract |
| [tools.md](tools.md) | `tools` plan: the view tool's milestones |
| [ui.md](ui.md) | `ui` as built: ImGui layer, debug menu, performance overlay, console |
| [perf-load.md](perf-load.md) | `perf` for loading: the load trace, measurements, what was done |
| [perf-frame.md](perf-frame.md) | `perf` for the running game: measurements, profile, plan |
| [compat.md](compat.md) | `compat`: what PR needs from the engine, done and missing |
| [upstream-fixes.md](upstream-fixes.md) | `fix` and standalone `build` changes, kept so they can be offered upstream |
| [flags.md](flags.md) | Every environment flag and build option this project adds |

The rest of `docs/` is OpenBF2's documentation, inherited as reference ([AGENTS.md](../../AGENTS.md#documentation)).

## History

- The realitymod OpenBF2 PR #1 (PR's game-logic port) arrived as one merge, `c4a3a850`. OpenBF2 is
  no longer merged in.
- Before 2026-11-17, commits used `feat:`, `fix:`, `compat:`, `build:` prefixes and this project's
  own lines carried `[CHOSEN, not recovered]` tags; those tags were then replaced by lane markers.
- Before 2026-11-19, milestones were called steps; commit messages of that time say "step 8".
