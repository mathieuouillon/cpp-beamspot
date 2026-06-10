# BeamSpot (C++/ROOT port)

CLAS12 beam-spot analysis, ported from the Java/groot `BeamSpot` (S. Stepanyan,
CLAS12 Note 2020-003). The analysis is **headless**: it reads CLAS12 DSTs with
the [hipo](https://github.com/gavalian/hipo) library, fits the target-window
position versus φ in θ bins, extracts the beam spot `(x0, y0, z0)`, and writes a
single self-describing ROOT file plus the physics/CCDB text deliverables. All
plotting lives in [`scripts/plot_beamspot.py`](scripts/plot_beamspot.py).

## Layout

```
include/beam_spot.h     data records (config/histograms/analysis) + free-function API
include/cli.h           header-only, argparse-style command-line parser
src/beam_spot.cxx       fill + analyze + I/O  (free functions, no graphics)
src/main.cxx            CLI + functional pipeline
external/hipo4/         vendored hipo library (built from source)
scripts/plot_beamspot.py   plotting (uproot + matplotlib + mplhep)
```

## Design

The analysis is written in a functional style: plain data records flow through
free functions, with no stateful analysis class.

```cpp
beamspot::config cfg{ {10,...,30}, fit_range, target_z, bins_per_sector };
beamspot::histograms h = beamspot::fill_dst(files, cfg, n_threads);  // or merge_files(...)
beamspot::analysis   a = beamspot::analyze(std::move(h), cfg);
beamspot::beam_spot_result r = beamspot::results(a);
beamspot::write_root(a, path); /* + write_results_txt / write_ccdb_table */
```

Multithreaded filling uses `ROOT::TThreadedObject<TH1F>/<TH2F>` — one histogram
per worker thread, merged with `TH1::Add` semantics (via a custom `SnapshotMerge`
function so the result is bit-identical to a single-threaded run). The per-track
cut/bin logic is a single higher-order `for_each_hit(...)` shared by the
sequential and parallel fill paths.

## Build

Requires ROOT (driven via `root-config`), plus **lz4** and **fmt** (for the
vendored hipo). hipo itself is built from source under `external/hipo4/`.

```sh
meson setup build
meson compile -C build
```

The project pins **C++17** to match the standard ROOT 6.38 was built with
(`root-config --cflags` reports `-std=c++17`). If your ROOT reports a different
standard, change `cpp_std` in [`meson.build`](meson.build).

- **lz4** is found via pkg-config and enabled with `-D__LZ4__` (CLAS12 DSTs are
  LZ4-compressed — without it hipo cannot read them).
- **fmt** is found via pkg-config/cmake if available, otherwise used header-only
  from `/opt/homebrew`, `/usr/local`, or `/opt/local`. Adjust the prefix list in
  [`meson.build`](meson.build) if fmt lives elsewhere.

## Run

```sh
./build/beamspot -o run_out path/to/rec_clas_*.hipo
```

| Short | Long | Default | Meaning |
|---|---|---|---|
| `-o` | `--output-dir` | `beamspot_out` | output folder (created if missing) |
| `-p` | `--prefix` | `BeamSpot` | filename prefix |
| `-r` | `--fit-range` | `1.0` | fit-range scale factor |
| `-z` | `--target-z` | `25.4` | nominal target/foil Z (cm) |
| `-n` | `--bins-per-sector` | `10` | φ bins per sector |
| `-j` | `--threads` | `0` | worker threads for the fill (0 = all cores) |
| `-m` | `--merge-histograms` | off | treat inputs as ROOT results files and `Add()` them |
|  | `--dry-run` | off | run the analysis but write no files |
| `-V` | `--version` | | print version |
| `-h` | `--help` | | usage |

Outputs (under `--output-dir`, with `--prefix`):
`BeamSpot_results.root`, `BeamSpot_results.txt`, `BeamSpot_ccdb_table.txt`.

Merge multiple runs without re-filling:

```sh
./build/beamspot -m -o merged run_a/BeamSpot_results.root run_b/BeamSpot_results.root
```

(or just `hadd` the histograms at the shell).

## Performance

The fill loop reads CLAS12 DSTs through a `hipo::chain`. With `-j 1` it runs a
single sequential pass; with `-j N` (or `-j 0` = all cores) it uses the chain's
record-level parallelism with `ROOT::TThreadedObject` histograms (one per worker)
merged with `TH1::Add` before the fit — the result is bit-identical to the
single-threaded run.

Measured on one 9.1 GB DST (598,738 events, run 22083), 12-core machine:

| implementation | event rate | wall time |
|---|---|---|
| Java (groot)        |  56 kHz |  10.8 s |
| C++ `-j 1`          | 100 kHz |   7.0 s |
| C++ `-j 0` (12 thr) | 146 kHz |   4.4 s |

C++ is ~1.8× faster than Java single-threaded and ~2.6× with all cores. Scaling
is sublinear because the work is dominated by disk I/O and LZ4 decompression
rather than the (cheap) per-track cuts and histogram fills.

## Plotting

Plotting uses **matplotlib + mplhep** (no PyROOT). Everything is read with
`uproot`; the fit *curves* are redrawn from the stored parameter trees
(`modulation`, `slice_fits`) rather than from `TF1`s, so PyROOT is never needed.

```sh
python3 -m venv .venv && .venv/bin/pip install -r scripts/requirements.txt
.venv/bin/python scripts/plot_beamspot.py -o run_out          # reads *_results.root
.venv/bin/python scripts/plot_beamspot.py -o run_out -f pdf   # PDF instead of PNG
```

Figures written into the output folder:

- `<prefix>_summary.png` — beam spot (x₀, y₀, z₀, φ₀, r₀) vs θ with the `pol0`
  constants (the headline figure), in mplhep ROOT style.
- `<prefix>_bin<i>.png` — z-vs-φ map per θ bin with the fitted target-z points
  and the `z₀ − A·cos(φ − φ₀)` modulation curve overlaid.
- `<prefix>_slices_bin<i>.png` — the accepted z slices in a θ bin, each with its
  gaussian + linear-background fit.

## The ROOT file

`BeamSpot_results.root` contains:
- `h1_z`, `h1_phi`;
- `h2_z_phi_<i>` (z vs φ per θ bin);
- `g_results_<i>` with the `z0 − A·cos(φ − φ0)` modulation `TF1` attached;
- `slices/bin_<i>/h_slice_*` z slices, each with its gaussian+linear fit;
- `gZ/gR/gP/gX/gY` summary graphs with their `pol0` fits;
- a flat `points` `TTree` (per-θ values + errors) and a single-row `final`
  `TTree` (the fitted beam-spot constants), both uproot-readable.

## Note: ROOT / Cling on macOS

On some macOS setups ROOT's Cling fails to JIT with:

```
.../cling/std_darwin.modulemap: error: header '__configuration/experimental.h' not found
```

This happens when Cling resolves libc++ from the **CommandLineTools** SDK (whose
older libc++ lacks that header) while ROOT's modulemap expects the **Xcode** SDK
libc++. It breaks *all* ROOT fitting/JIT/PyROOT — not just this tool. Point Cling
at the Xcode libc++:

```sh
export ROOT_INCLUDE_PATH=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/include/c++/v1
```

(Permanent fixes: update/reinstall CommandLineTools, or rebuild ROOT.)
