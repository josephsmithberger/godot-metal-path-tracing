# Metal RT orbit benchmark

This is the reproducible workload behind the table in the repository README.
It renders a deterministic procedural scene at 1920×1080 while the camera
orbits, which exercises the path tracer's scene updates instead of measuring a
static image alone.

Run it from the repository root after building the Apple Silicon editor:

```bash
./benchmark/RUN\ BENCHMARK.command
```

The runner prefers a release-package `Godot Metal RT.app`; otherwise it uses
`bin/godot.macos.editor.arm64`. Set `GODOT_METAL_RT_BIN` to test another binary.
All generated CSVs, system reports, and raw logs go under `benchmark-results/`
and are ignored by Git.

For a short validation pass rather than the full 18-run matrix:

```bash
GODOT_BENCHMARK_QUICK=1 ./benchmark/RUN\ BENCHMARK.command
```

Please include machine model, chip, macOS, source commit, exact configuration,
and a concise result table when submitting a benchmark PR. Do not add binaries,
`.godot/`, or raw logs.
