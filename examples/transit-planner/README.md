# Transit planner project

This is a complete GTI project rather than a single-feature snippet. It reads
a transit graph from `data/network.txt`, validates the input, computes the
shortest route with Dijkstra's algorithm, verifies the result, and renders a
human-readable report.

The project exercises:

- a `gti.toml` executable target with distinct development and release
  profiles;
- multiple GTI source units with direct includes;
- unbuffered file I/O and explicit `expected<T, E>` error handling;
- classes, mutable methods, a tracked read-only reference field, and scoped
  enums;
- checked fixed-width arithmetic, fixed arrays, indexing, and loops;
- a real shortest-path algorithm and route reconstruction; and
- reusable reporting functions over computed results, including direct
  fixed-width integer output through `std::print`.

From this directory, inspect and analyze the project without compiling:

```sh
../../build/gti metadata --format json
../../build/gti check
```

Build or run the default development profile:

```sh
../../build/gti build
../../build/gti run
```

Run the optimized release profile:

```sh
../../build/gti run --release
```

Expected output:

```text
GTI Transit Planner
Loaded 12 bidirectional links from data/network.txt

Fastest route:
Depot -> Museum -> Market -> University -> Stadium -> Observatory
Total travel time: 11 minutes
Stations visited: 6
```

Project artifacts are written beneath
`build/gti/<profile>/<target-triple>/`. Run `../../build/gti clean` from this
directory to remove only that GTI-owned build subtree.
