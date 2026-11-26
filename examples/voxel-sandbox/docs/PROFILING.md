# Profiling and benchmarking

This page covers what the voxel sandbox measures and how. It also covers how
to run the benchmarks, the report format, and how to compare the results with
another implementation or a commercial voxel game without overstating what the
numbers show.

## What is measured

All CPU timing comes from `std::chrono::steady_clock` (`voxel::now_ns()`, int64
nanoseconds). The profiler lives in `src/core/profiler.gti`.

### Frames

`frame_begin()` / `frame_end()` bracket one iteration of the game loop: input,
simulation, world streaming, rendering, HUD and `glfwSwapBuffers`. The frame
time is the wall time between the two calls. The loop places them so that
nothing significant falls outside them, which makes the frame time the
frame-to-frame interval that a player experiences. With vsync on, the swap
blocks, and the frame time is capped at the display interval. Benchmarks
therefore always run with vsync off.

### Zones

CPU zones split each frame into parts. Each zone pair costs two clock reads
and an add. A zone may open and close several times in one frame; the
durations accumulate.

| Zone | Covers |
| --- | --- |
| `input` | GLFW event polling, input state, camera update |
| `physics` | player movement, collision, block ray cast |
| `worldgen` | terrain/biome/cave/ore/tree generation of new chunks |
| `meshing` | face culling, ambient occlusion and greedy merging into vertex arrays |
| `upload` | `glBufferData` of new or changed chunk meshes |
| `cull` | reserved; frustum culling and draw sorting currently run inside `render` |
| `render` | issuing the opaque, cutout and water passes (CPU side of GL calls) |
| `hud` | building and drawing the overlay |
| `swap` | `glfwSwapBuffers`. With vsync off on macOS this mostly measures how long the driver makes the CPU wait for the GPU |
| `stream` | chunk streaming bookkeeping: range scans, revision checks, unloading |
| `stats` | GPU timer polling and the once-per-second statistics |

`untracked_ms` in the report is the frame average minus the sum of zone
averages. It is loop overhead and everything not inside a zone.

World generation runs as one parallel batch per frame (`--batch` chunks on
`--threads` workers), and meshing runs under a per-frame budget (`--budget-ms`). Their
zone times therefore show how much of each frame went to streaming, not how
fast a chunk is generated. For per-chunk throughput, use the headless benchmark.

### GPU time

The renderer brackets its world passes with `GL_TIME_ELAPSED` queries, using a
small ring of query objects. It reads each result a few frames later, once
`GL_QUERY_RESULT_AVAILABLE` is set, so the CPU never stalls on the GPU. Each
result goes to `Profiler::record_gpu_ns()`, which attributes it to the frame
that was current when the result arrived. The GPU number is therefore an
average over the run, not an exact per-frame figure. The CSV's `gpu_ms` column
is empty for frames that received no result. On Apple Silicon the OpenGL
driver is a translation layer over Metal. Timer queries work, but treat them
as approximate.

### Counters

Counters are added per frame (`count(counter, value)`). The profiler keeps the
last frame's value, a run total, and the run maximum:
`chunks_generated`, `chunks_meshed`, `chunks_visible` (after frustum culling),
`draw_calls`, `quads_drawn` and `chunks_loaded` (resident chunks).

### Frame-time statistics

The statistics are computed over every recorded frame of the run (the report)
or over the last wall-clock second (window title and F3 overlay):

- **fps avg** = frames / sum of frame times. This is a time-weighted average,
  not the mean of per-frame fps values.
- **avg / median / p95 / p99 / p99.9 / max frame ms.** Percentiles use the
  *nearest-rank* method: the smallest sample such that at least p% of the
  samples are less than or equal to it. Nothing is interpolated, so every
  percentile is a frame that actually occurred.
- **1% low and 0.1% low fps** = 1000 / (mean frame time in ms of the slowest
  1% or 0.1% of frames), with a minimum of one frame. This is the common
  "average of the worst frames" definition. Some tools instead report
  1000 / p99 as the "1% low". That figure is never lower than the one here,
  so check which definition a number uses before comparing. Both inputs are in
  the report: `frame_ms.p99` and `low_1pct_fps`.
- **Per-zone avg ms and share.** Share is the zone average divided by the
  frame average.

Storage is three flat `int64` vectors (frame total, zones, GPU). They are
reserved for 16k frames and then grow amortized, so a steady-state frame does
not allocate. Sorting happens only once per second for the overlay and once
for the report.

## Benchmark modes

### Interactive flight benchmark

```sh
gti run voxel-sandbox --release -- --benchmark [--seconds N] [--render-distance R] [--report PATH]
```

This mode disables vsync, captures the mouse and flies the camera along the
deterministic path in `src/bench/flight.gti`, the same path every run. The
default path is 60 s long, at altitude 85–100 (sea level is 48):

| Time | Motion | Stresses |
| --- | --- | --- |
| 0–25 s | straight leg toward −Z at 24 blocks/s (~600 blocks) | chunk generation, meshing and upload every frame, like a player crossing new ground |
| 25–40 s | wide 90° right turn (radius ~230 blocks) | streaming while the view direction changes |
| 40–44 s | slows to a 3 blocks/s drift | — |
| 42–58 s | slow 360° pan, looking down and back up | frustum culling and overdraw in every direction; the world is now mostly resident |
| 58–60 s | settle | — |

Speed, heading, look direction and pitch are smooth functions of time. The
position is integrated at 240 Hz and interpolated, so the path does not depend
on the frame rate. A run at 30 fps and a run at 300 fps visit the same places
at the same times. A slower build does less work per second of path, not more.
Compare frame times, not totals.

Recommended protocol: build with `--release`, close other applications, plug
in the laptop, let the machine cool, use the same window size every time, and
run 3–5 times. Report the median run, and the spread across runs.
`--seconds N` flies the same path in N seconds (`FlightPath(double seconds)`): the
same route, at a different speed. The default is 60 s.
The world seed is fixed (default 1337) unless `--seed` is given, and it is
recorded in the report's meta.

### Headless benchmark (`voxel-bench`)

```sh
gti run voxel-bench --release -- [--seed N] [--radius R] [--reps K] [--threads T] [--json PATH]
```

This mode opens no window and uses no GPU. It generates and meshes every chunk
within radius R, K times, and reports per-phase timings (generation, plain
meshing, greedy meshing) and a checksum. It measures the language and compiler
on the game's real kernels, without drivers and vsync adding noise.

### GTI vs C++ (`bench/compare.py`)

`bench/reference/voxel_bench.cpp` translates the same kernels algorithm for
algorithm: same data layout, loops and containers. It must produce the same
blocks, quads and checksum. Build it with `clang++ -O3 -std=c++20
-ffp-contract=off`; the last flag matches GTI's per-operation binary32
rounding, which does not fuse operations into FMA. `bench/compare.py` builds
both, runs them alternately several times, checks that the checksums match,
and prints per-phase ratios. Notes:

- GTI checks integer overflow and bounds by default, and its release build
  keeps those checks. The C++ port does not check, so the ratio includes the
  cost of GTI's safety checks. That is a fair comparison of "safe GTI as
  shipped" against "plain C++".
- Only use runs where both checksums agree.

## Report format

`write_report(path)` writes `path` (JSON) and a sibling CSV
(`x.json` → `x.csv`; any other name gets `.csv` appended).

```json
{
  "schema": "voxel-sandbox-profile/1",
  "meta": { "seed": "1337", "render_distance": "12", "gl_renderer": "Apple M2", "build": "release", "...": "..." },
  "frames": 7213,
  "duration_s": 60.0021,
  "fps_avg": 120.213,
  "frame_ms": {"avg": 8.3187, "median": 7.9012, "p95": 11.204, "p99": 15.881, "p99_9": 24.410, "max": 31.007},
  "low_1pct_fps": 55.120,
  "low_0_1pct_fps": 36.830,
  "zones": {
    "input":    {"avg_ms": 0.0310, "share": 0.0037},
    "physics":  {"avg_ms": 0.0050, "share": 0.0006},
    "worldgen": {"avg_ms": 2.1000, "share": 0.2524},
    "...": {}
  },
  "untracked_ms": 0.0800,
  "gpu_ms_avg": 3.2100,
  "gpu_samples": 7205,
  "counters": {
    "chunks_generated": {"total": 3120, "per_frame_avg": 0.433, "max": 4},
    "...": {}
  }
}
```

(The numbers above are illustrative, not measurements.)

- All values in `meta` are strings and are free-form. The game is meant to record at
  least the seed, render distance, window and framebuffer size, vsync, worker
  threads and batch size, and GL renderer and version strings. The build profile
  and compiler version are not known at run time; record them with the
  result (`gti --version`, `--release`).
- `gpu_ms_avg` is `null` when no timer result arrived.
- CSV columns: `frame,total_ms,input_ms,physics_ms,worldgen_ms,meshing_ms,upload_ms,cull_ms,render_ms,hud_ms,swap_ms,stream_ms,stats_ms,gpu_ms`,
  one row per frame, times in ms with 4 decimals. `gpu_ms` is empty when a
  frame received no GPU result.

## Comparing against a commercial voxel game

A fair comparison is hard. The goal is to say *how close* this engine comes
under matched conditions, and to state plainly where the workloads differ.
The method below uses Minecraft: Java Edition as the example. It is the most
widely measured game in this genre and ships its own profiling tools. Command
names and hotkeys were checked against the Minecraft Wiki in November 2026.
They change between versions, so re-check them for the version you test.

### Settings to match

| Setting | Voxel sandbox | Minecraft Java |
| --- | --- | --- |
| Render distance (chunks) | `--render-distance R` | Video Settings → Render Distance = R |
| Simulation distance | n/a (we tick nothing) | set to the minimum (5) to reduce ticking cost; it cannot be removed |
| Resolution / window | `--size WxH` or `--fullscreen` | same window size, or fullscreen on both |
| VSync | off (`--benchmark` forces it) | VSync off |
| Frame cap | none | Max Framerate: Unlimited |
| Graphics | — | "Fast", clouds off, smooth lighting **on** (we also do ambient occlusion), biome blend low, entity shadows off, particles minimal |
| Mods / shaders / resource packs | none | none (vanilla). Note whether a performance mod such as Sodium is used, and report it as a separate configuration |
| Machine | same machine, same power state, same display | same |
| World | fixed seed, flight path | a new world (seed recorded) in Spectator mode, flying a comparable straight line at a comparable speed, then a pan |

Java Edition runs on the JVM. Let it warm up: load the world, fly for 1–2
minutes, and only then start the measured run, because the JIT compiler and
GC heap sizing settle during that time. Also record the JVM version and heap
settings (`-Xmx`).

### What Minecraft reports, and how to capture it

- **F3 debug screen.** The FPS line shows the current frame rate, the frame
  cap (`T: inf` when unlimited) and the present mode. The chunk line includes
  `C: rendered/total` chunks (comparable to our `chunks_visible` and
  `chunks_loaded`) and `D:`, the effective render distance.
- **F3 + 2** (Alt+F3 before 23w33a) shows the frame-time chart: the last 240
  frames with min/avg/max frame time. It is good for spotting hitches, but it
  is not a full-run statistic.
- **F3 + 1** (formerly Shift+F3) shows the live profiler pie chart, broken
  down by game subsystem.
- **F3 + L** records a 10-second profile in single player and writes a ZIP
  under `.minecraft/debug/profiling/`. This is the client-side counterpart of
  `/perf`.
- **`/perf start` / `/perf stop`** record up to 10 s of profiler and metrics
  output (a ZIP with `profiling.txt`, tick-time and metrics CSVs) to
  `debug/profiling`. According to the wiki it runs **only on a dedicated
  server** (permission level 4). It measures server ticking, not client
  rendering, so it is not useful for an fps comparison.
- **`/debug start` / `/debug stop`** run the time profiler (permission level 3).
  `stop` reports the average TPS for the session. Since 1.17 the detailed
  results moved to `/perf` and F3 + L. This is also tick-side data, not
  rendering data.

None of these tools exports a per-frame frame-time log for a whole run. To get
percentiles and 1%/0.1% lows that are comparable to our report, use an
external frame-time capture tool that works for the graphics API the game uses
on that platform. Examples are PresentMon or CapFrameX on Windows, and Xcode
Instruments or the Metal performance HUD on macOS where they apply. Then
compute the same statistics from that tool's frame log: nearest-rank
percentiles, and lows as the mean of the slowest frames. Use F3 + 2 only as a
sanity check.

### What is and is not comparable

Roughly comparable, with caveats:

- **Frame-time distribution while streaming new terrain** at the same render
  distance, resolution and vsync-off settings on the same machine. Report avg,
  p99, and 1%/0.1% lows.
- **Steady-state frame time** while panning over already-loaded terrain at the
  same render distance. This mainly compares draw submission and culling.

Not comparable, or only after explanation:

- **World volume.** Chunks are 16×384×16 in Minecraft (since 1.18) and
  16×128×16 here. At the same render distance, Minecraft has 3× the volume per
  column to generate, store and potentially mesh. Its sections are 16³, and
  empty sections are cheap, so the real cost difference is smaller than 3×,
  but it is not zero. For a closer match, compare at equal *loaded block
  count* as well as at equal render distance, and say which one you used.
- **Lighting.** Minecraft propagates sky light and block light and recomputes
  them when blocks change. We use per-vertex ambient occlusion and directional
  shading only.
- **Simulation.** Minecraft runs an integrated server even in single player:
  20 TPS of entities, mobs, block ticks, random ticks, fluid flow, redstone and
  chunk saving. We have no entities and no ticking world, and we do not save.
- **Generation complexity.** Its world generator (3D noise density functions,
  aquifers, structures, features, carvers) does far more work per chunk than
  ours. Chunk-generation throughput is therefore not an apples-to-apples
  number in either direction.
- **Rendering features.** These include its fog, sky, clouds, entities, block
  entities, animated textures, mipmapping and anisotropy settings, and its
  translucency sorting. Its renderer is OpenGL; ours is OpenGL 4.1 through
  Apple's Metal translation layer on macOS, as is Java Edition's on a Mac.
- **Runtime.** JIT warm-up, GC pauses (they show up in the 0.1% lows) and a
  multithreaded chunk builder on its side; on ours, an AOT-compiled binary with
  chunk generation in parallel batches on worker threads (`--threads`,
  default 4; `--batch`, default 8 chunks per frame) with meshing on the main
  thread. Say how many threads each side used.
- **Process memory** is not comparable: JVM heap reservation is not working
  set.

The fair headline is therefore something like *"at render distance R on
machine M, the GTI sandbox renders its (simpler) world at X fps avg / Y fps
1% low; vanilla Minecraft Java V renders its (richer) world at X' / Y'"*. It
must not be *"GTI is N× faster than Minecraft"*. The headless GTI-vs-C++
comparison is the like-for-like language measurement. The commercial
comparison only places the result in context.

### Results table template

Record one row per configuration. Use the median of 3–5 runs, and give the
spread across runs in parentheses.

| Date | Machine / OS | Engine & version | Build / JVM | Window | RD (chunks) | Vsync | Scenario | fps avg | median ms | p99 ms | 1% low fps | 0.1% low fps | GPU ms | Chunks visible / loaded | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| | | GTI sandbox (gti 0.494.0) | release -O3 | 1920×1080 | 12 | off | flight 60 s | | | | | | | | |
| | | GTI sandbox | release -O3 | 1920×1080 | 12 | off | pan only (42–58 s) | | | | | | | | |
| | | Minecraft Java x.y.z | Java 21, -Xmx4G | 1920×1080 | 12 | off | spectator flight, warmed up | | | | | n/a | | | capture tool: |
| | | Minecraft Java x.y.z + Sodium | | 1920×1080 | 12 | off | same | | | | | n/a | | | |

For the headless kernels:

| Date | Machine | Radius | Reps | Threads | Phase | GTI ms | C++ ms | GTI / C++ | Checksums match |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| | | 8 | 5 | 1 | generate | | | | |
| | | 8 | 5 | 1 | mesh (greedy) | | | | |
