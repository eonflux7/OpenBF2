# Tools

The `tools` lane: an in-game test harness for renderer work, called the **view tool**. It replaces
the manual loop (load, spawn menu, random spawn point, fly to the spot, look, screenshot) with
saved views, view sets, camera paths and unattended runs that write a report. Rules:
[AGENTS.md](../../AGENTS.md#what-this-fork-is).

- **Console first.** Everything the tool does is a console command or variable. The **Views** tab
  of the debug menu ([ui.md](ui.md)) is one frontend; `BF2_DEVCMD_FILE` ([flags.md](flags.md)),
  scripts and config files are the others. An agent and the owner drive the same commands.
- **Results are files.** Screenshots, a JSON report and a short Markdown summary per run, so a run
  can be read without watching it. Diffs and contact sheets are made from those files.
- **Idle costs nothing.** While no view, path or run is active, frames draw and cost the same as
  without the tool.
- **Map data is the owner's.** View files live in the mod's `settings/` directory, outside this
  tree, like `eon64-commands.txt`. Debug shading modes live with the shaders, outside this tree; the
  engine only provides the variable that selects them.
- It comes before renderer step 9 (owner, 2026-11-19): milestones 1–3 are done first, so step 9 is
  measured against reference views and a benchmark path.

## Milestones

| Milestone | Commit scope | Status |
|---|---|---|
| 1. Views | `tools(view)` | Planned |
| 2. Unattended runs and regression | `tools(run)` | Planned |
| 3. Paths and benchmark | `tools(bench)` | Planned |
| 4. Renderer inspection (alongside render step 9) | `tools(inspect)` | Planned |
| 5. Soldier mode and capture | `tools(soldier)` | Planned |

Starting set (owner): about 5 views and one benchmark path each on Muttrah City and Saaremaa.

### 1. Views

**Camera override.** The tool takes the render camera after the game sets it and before culling,
so culling, LOD, streaming and shadow cascades follow the override. No soldier is spawned: it
works from the spawn screen, with the spawn menu and HUD hidden while a view is active. Leaving the
view hands the camera back to the game.

**Free fly.** A key toggles a free camera: no collision, mouse look, adjustable speed, mouse wheel
for FOV. This is how views are placed.

**A view** is a name, position, yaw, pitch, FOV, and options:
- console lines run on entering it (debug mode, a forced cascade, a time of day) and undone on
  leaving;
- a note (for example "fence alpha flicker, check after step 10"), shown in the menu and copied
  into reports;
- settle frames (default below).

**A set** is an ordered list of views with a dwell time each. Each map has one view file,
`<mod>/settings/eon64-views/<map>.txt`:

```
# views for muttrah_city_2
view market   pos=812.4,62.0,-311.9 yaw=134.5 pitch=-8 fov=75 | rooftop alpha fences
  run renderer.sunShadowDebug 1
view airfield pos=-1203.0,48.5,902.2 yaw=12.0 pitch=-3
set default market airfield
set quick   market
```

`view` takes `key=value` fields (`fov` defaults to the game's); indented `run` lines belong to the
view above them; `| text` is the note; `set NAME VIEW...` lists views in order; `#` starts a
comment. The menu rewrites the file when views change; hand edits are kept if the menu is not
used, and Reload rereads the file.

**Commands** (provisional names): `view.go NAME`, `view.save NAME [note]` (the current camera),
`view.delete NAME`, `view.leave`, `view.list`, `view.reload`, `view.fly 0|1`,
`view.shot [NAME]`.

**Views tab:** the list with Go, Update (to the current camera), Delete and reordering; set editing;
Save current view; the note and run lines of the selected view.

**Bookmarks:** Ctrl+F1..F8 store the current camera, F1..F8 go back to it, for quick A/B looks
without naming views. Not saved.

**Screenshot in the engine.** The scene is read back before the HUD (or with it, by option) and
written as PNG, plus a 960-wide JPEG for quick review. It works when the window is covered.

**Settle and freeze.** Before a shot the tool waits until streaming has no pending loads, LOD
crossfades have finished and the shadow cascades are filled, then a fixed number of frames
(`tools.settleFrames`). While a view is active, the renderer's time inputs are frozen: wind on
overgrowth, water, particle updates, the sun. Whether the game simulation can also be paused on a
local server is part of this milestone; if it cannot, the report says what was not frozen.

**Stamping.** Every shot and report records the engine commit, the shader set's commit, map, game
mode, resolution, MSAA, and the `renderer.*` variables that differ from their defaults.

Accepted when: views are placed with free fly and saved from the menu, `view.go` from the console
and from `BF2_DEVCMD_FILE` reaches the same image, and two shots of the same view in one session
are identical (the **determinism check**; if a source of difference cannot be frozen, it is named).

### 2. Unattended runs and regression

**Run.** `view.run SET [label]` visits each view of a set, settles, writes the screenshot and the
GPU pass timings, draw calls and primitives for that view, then a report. `BF2_VIEWRUN=<set>[:label]`
does the same right after autohost and quits when done, so one launch line runs a set unattended.

**Report.** In `<output>/<map>/<label>/` (`tools.outputDir`): the PNGs and JPEGs, `report.json`
(the stamp, per view: pass timings, frame time, draw counts, the load gate, errors) and `report.md`
(the same, short).

**Load gate.** During the load the tool counts shader compile errors, missing techniques and effects
that failed to load; they head the report and fail the run. `renderer.compileAllEffects` compiles
every technique of every loaded effect, not only those the first frames use (night variants,
vehicle-only and cockpit shaders), and logs each failure.

**Baselines and diffs.** A label marks a baseline (for example `step8`, `stock`). A script in this
repository compares two runs: per view a difference metric with a tolerance, a heatmap, and the
change in pass timings and draw counts; it writes one contact sheet image (thumbnails and heatmaps)
and a summary table. One image per comparison is what gets reviewed.

**Stock baseline.** The same set run with the stock PR shader set gives a reference for drift from
the original look, alongside the previous step's baseline.

**Reload and diff.** `view.reloaddiff` shoots the current view, reloads the shaders, waits the
settle frames, shoots again and writes one A/B image with a heatmap: the shader edit loop in one
command.

**Watchdog.** On a crash, hang or device loss the last report entry names the view and command that
were running, and the crash address is symbolized from the RendDX9 map file.

Accepted when: an unattended run on Muttrah City and on Saaremaa produces a report and contact
sheet, a second run diffs clean against the first, and a deliberate shader error fails the load
gate.

### 3. Paths and benchmark

**A path** is a list of keyframes (view fields plus a time), played on a Catmull-Rom spline for
position and smoothed for angles and FOV. It is stored in the map's view file as `path NAME` with
indented `key TIME pos=… yaw=… pitch=… fov=…` lines. The Views tab adds a keyframe at the current
camera, previews the path and draws it in the world while editing.

**Two clocks.** Real time for benchmarks (frame times as they are); fixed step for images (frame N
looks the same in every run). `path.play NAME [real|fixed]`, `path.stop`.

**Benchmark.** `bench.run PATH [runs] [label]` plays the path in real time, after one warm-up pass,
the given number of times. It writes a per-frame CSV (frame, CPU, GPU, per pass, draws) and a
summary: average, 1 % low, worst frame, per-pass averages, CPU or GPU bound, and the spread between
runs. Each labelled benchmark appends its summary to a per-map history file, so frame time and draw
counts can be charted across steps.

**Hitch log.** During paths and benchmarks (and in normal play with `tools.hitchLog 1`), frames over
a threshold are logged with what happened in them: an effect compiled at run time, textures
streamed in, effects created.

**Perf map.** `bench.map [step]` samples the map on a grid at four headings, unattended, and writes
frame time and draw counts per cell as an image over the minimap and as a table. Any cell can be
saved as a view, so the worst places on a map are found rather than guessed.

Accepted when: the Muttrah City and Saaremaa benchmark paths give a spread between runs small
enough to measure step 9 with (stated in the report), and a perf map of one map is produced.

### 4. Renderer inspection

Runs alongside render step 9; parts may be step 9's first sub-steps.

- **Freeze culling.** `tools.freezeCull 1` keeps culling and LOD selection at the current camera
  while the camera moves on, to see what was submitted.
- **Draw breakdown** per view and in the overlay: draws and primitives for terrain, static meshes,
  bundled meshes, overgrowth, roads, decals and each shadow cascade, and the most expensive objects.
- **Debug shading modes** (`renderer.debugView`): albedo, normals, lighting only, cascade colours,
  LOD level colours, overdraw, wireframe, mip level. The shaders implement the modes.
- **Pass toggles:** skip a pass and see the frame and the timings change, live.
- **Pass markers** (`D3DPERF_BeginEvent`) named like the GPU spans, so captures read by pass.
- **Pixel inspector:** the colour, depth and linear depth under the cursor, the normal in debug
  modes, and a luminance histogram (for auto-exposure, render step 12).
- **Overrides:** time of day, sun angle, fog.
- **Variable sweep:** `view.sweep VAR FROM TO STEP` shoots the current view per value and writes
  one strip with the timings.

### 5. Soldier mode and capture

- **Soldier mode:** the local server spawns a kit at a view's position and heading, so views can
  show the first-person model (toggled per view) and its shaders. Optionally a vehicle at the view
  (cockpits, vehicle shaders). How the server is told to spawn and place the soldier is
  investigated first.
- **Capture at a view:** `view.capture NAME` goes to the view, settles and triggers a RenderDoc
  capture through RenderDoc's in-process API.
- **Load stress:** spawn vehicles and soldiers at a view, or trigger explosions and smoke, for the
  cost of skinned meshes and particles.
- **Settings matrix:** run a set under several profiles (antialiasing, resolution, shadow quality).
- **Temporal metric:** on a slow fixed-step path, the difference between consecutive frames, so
  LOD popping, shadow shimmer and crossfade flicker become a number.
