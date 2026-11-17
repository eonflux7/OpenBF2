# Agent instructions: openbf2-eon64

This branch is a fork of OpenBF2 (kr4uzi/OpenBF2, itself downstream of B2BF/OpenBF2). It is the
canonical policy for every coding agent working on this branch; `CLAUDE.md` imports it. If guidance
conflicts, the user's current request wins, then this file, then `docs/eon64/`.

Upstream's own policy (a fidelity-first reconstruction of Battlefield 2 1.50 with IDA/Ghidra evidence
rules) does **not** govern this branch as a whole. It applies only when preparing a change for
upstream; see [Upstreaming](#upstreaming). Read it with `git show kr4uzi/main:AGENTS.md`.

## What this fork is

An x64 Battlefield 2 client that runs the Project Reality mod (PR) and modernizes its renderer.
Seven kinds of work happen here. They are called **lanes**, and every change belongs to exactly one.

| Lane | What it is | Rule | Destination |
|---|---|---|---|
| `fix` | A bug in the reconstructed engine (crash, wrong behaviour, x64 hazard) | Smallest correct change. Match retail where the evidence exists; say so when it was not checked | Upstream (kr4uzi / B2BF) |
| `compat` | What PR's content, scripts or launcher need from the engine | Specified by what PR does, never by guesswork about rhooks internals. No visual changes | Possibly realitymod |
| `render` | The renderer uplift: new design, not retail | Clean rewrites. No retail constraint. No legacy path kept beside a new one once the step is accepted | Private to this fork |
| `perf` | Load times and throughput: threading, caching, avoiding repeated work | Changes when and on which thread work happens, never what it produces: the loaded state matches a serial load. While a step is developed, a flag forces the serial path for comparison; it is deleted when the owner accepts the step | Upstream where it stands alone; otherwise this fork |
| `build` | Toolchain, CMake/Ninja, build scripts, ABI checks | Never changes runtime behaviour | Mostly upstream |
| `dev` | Diagnostics and developer hooks (traces, autohost, file monitor) | Off by default; when unset, behaviour and cost are unchanged | Case by case |
| `upstream` | Imported code: upstream merges, realitymod's PR #1 game-logic port | Not edited in place. A bug in it is a `fix` | — |

When a change seems to need two lanes, split it into two commits. A `fix` found during `render` work
(for example the StateManager MRT slot overflow) is committed on its own, before the work that needed it.

## Repositories and what is public

- This repository (`eonflux7/openbf2-eon64`, branch `main`) is **public**. Only engine code and
  engine-side documentation go here.
- PR's shaders, decompiled PR scripts, PR's private Python shims and anything else derived from PR's
  closed content live in **private** repositories and directories outside this tree. Never copy them
  here, and never quote them in commits, comments or docs. Describing an engine-side contract that
  PR content relies on (a console command, an event id, a sampler slot) is fine.
- The machine-specific workspace (game installs, the PR overlay, the build VM, launch scripts) is
  documented outside this repository. Do not add personal paths or account details to this tree.

## Hard rules

- Never modify the original game installations; work only through the overlay.
- Stay offline: no online PR login, master server or launcher authentication.
- **Port 16567 is reserved** for the owner. Any agent-run server uses another explicit port.
- Do not create or submit pull requests, and do not reply to reviewers. Commit and push the fork
  branch only when asked or when it is part of an agreed workflow.
- Do not restart the running game without asking. When asked to check or screenshot, do it at once.

## Building and testing

- The Windows client builds in the build VM with MSVC (x64, C++14). The CMake/Ninja build compiles
  `BF2.exe` and the four plugin DLLs (RendDX9, TextureAtlasBuilder, SwiffPlayer, BF2Audio)
  incrementally; each plugin link runs its ABI check (`Code/Build/plugin_abicheck.cmake`), which
  removes the DLL on failure. The `.cmd` scripts under `Code/Build` read the same source manifests
  (`Code/Build/<plugin>_sources.txt`) and remain usable; keep both in step.
- Every compile line that touches `Code/BF2` states its image: `/DDEDICATED=0` for the client and
  its DLLs, `/DDEDICATED=1` for the server, always with `/FI Code/BF2/Core/BuildConfig.h`.
- **Done means seen in game.** A build or ABI check proves the image links; a `render` or `compat`
  change is done only when the owner has seen it work in game. Say exactly what was and was not
  exercised: built, ABI-checked, loaded, in game, which map, which flags.
- x86 is not built on this branch. Do not claim x86 results.

## Commits

```
<lane>(<area>): <what changed>

<why; what was tested and how; the flag it sits behind, if any>

Co-Authored-By: ...
```

Examples: `render(csm): blend between cascades`, `perf(effects): compile on worker threads`,
`fix(ai): guard getIsland(-1) on x64`, `compat(console): SquadManager.listSquads rows`,
`build(plugins): Ninja targets`, `dev(console): BF2_CONSOLE_TRACE`.

Changes that belong to no lane (documentation, this file, repository-wide marker renames) use
`docs:` with no area.

One lane per commit. Upstreamable `fix` and `build` commits must not depend on `render` code, so
they can be cherry-picked onto upstream alone.

## Source markers

Upstream marks reconstruction provenance with `[CHOSEN, not recovered]`, `[FRONTIER]`, `[INFERRED]`
and similar tags. Leave those alone in upstream code; do not add `[CHOSEN …]` for work on this branch.

Mark our changes with the lane instead, only where they sit inside code that otherwise follows
retail, and state why in the same comment:

- `[FIX]` a deliberate departure from retail behaviour to fix a bug; say what retail does.
- `[COMPAT]` behaviour a mod needs that retail does not have.
- `[RENDER]` a renderer change; new renderer files say so once in their header instead.
- `[PERF]` a load-time or throughput change; say what the serial code did.
- `[DEV]` a diagnostic or developer hook.
- `[BUILD]` in build files, where a choice differs from upstream's build.

A marker is a pointer, not a justification: the comment beside it still explains the reason.

## Feature flags

- Every flag this fork adds is listed in `docs/eon64/flags.md` with its lane and removal condition.
- A `render` flag exists only while its step is being developed. When the owner accepts the step
  in game, the flag and the replaced path are deleted in the same change.
- A `perf` flag selects the old serial path while its step is developed, and goes when the step
  is accepted, as for `render`.
- `dev` flags are permanent, off by default, and inert when unset.
- `compat` flags gate behaviour retail BF2 must not see (for example `BF2_HUD_AUTOVAR`).

## Documentation

| What | Where |
|---|---|
| Fork overview, lanes, workflow | `docs/eon64/README.md` |
| Renderer architecture, passes, targets, sampler slots, shader contract, roadmap | `docs/eon64/renderer.md` |
| PR compatibility: what PR needs from the engine and what is done | `docs/eon64/compat.md` |
| Load times: measurements, plan, what is done | `docs/eon64/perf.md` |
| Fixes worth upstreaming, with commits | `docs/eon64/upstream-fixes.md` |
| Flags | `docs/eon64/flags.md` |

Update the relevant document in the same commit as the change it describes. Commit messages
record history; the documents record the current state.

## Upstreaming

To prepare a `fix` or `build` change for upstream, cherry-pick it onto `kr4uzi/main` on its own
branch. From that point upstream's policy applies (`git show kr4uzi/main:AGENTS.md`): its evidence
vocabulary, binary evidence for recovered behaviour, and the linked-issue requirement for intentional
changes. Note in `docs/eon64/upstream-fixes.md` what was and was not checked against the retail binary.
The human author submits; agents do not.

## Merging upstream

Merge `kr4uzi/main` into this branch; never rebase published history. On conflicts in this file,
keep this branch's version and carry over any upstream rule that matters to `fix` work.
