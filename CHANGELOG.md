# Changelog

What changed in this repository, newest first, and what is being worked on right now. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). There are no version tags; entries
are dated. Measured numbers live in the [results log](docs/results-log.md), not here.

## Work in progress

- **Finding where the single-process speed-up of v0.10.0 comes from.** A round trip in one process
  is about 23 % faster than at v0.5.0-alpha (see 2026-11-14 below). The notification path reads the
  same in both versions (it moved from `NotifyEventHandler` to `MessagePassingServiceInstance`, same
  algorithm), so the next step is a `perf` profile of both binaries rather than more reading.
- **Upstream.** [eclipse-score/scrample](https://github.com/eclipse-score/scrample) still pins
  v0.5.0-alpha. Its update PRs stall (the repository has no CODEOWNERS and no owning team in the
  S-CORE project plan). The plan is to ask on the S-CORE channels whether scrample is still
  maintained before offering the fixes found here.

## 2026-11-14

### Added

- [Results log](docs/results-log.md) entry and [section 7 of the results page](https://intilinux-eng.github.io/scrample-pingpong-bench/#versions):
  v0.10.0 against v0.5.0-alpha on the same laptop, both binaries alternated, the whole comparison
  made twice. Data in `site/data/compare.json`.
- `tools/summarise_results.py --js-var`, so a page can load more than one data file.

### Changed

- **S-CORE platform release v0.10.0** instead of v0.5.0-alpha: `score_communication` 0.1.1 → 0.6.0,
  `score_baselibs` 0.1.3 → 0.2.17, the versions in `known_good.json` of
  [reference_integration v0.10.0](https://github.com/eclipse-score/reference_integration/releases/tag/v0.10.0).
  The versions now follow reference_integration rather than scrample.
- `platforms` 1.1.0, `rules_cc` 0.2.25 and `rules_python` 1.8.5, the versions communication 0.6.0
  resolves to anyway.
- `trlc` override moved to the commit reference_integration uses. `score_tooling` 3.x needs
  `subrule_trlc_image_stage`, which trlc 2.0.2 does not have; the old commit failed the build during
  analysis.
- Bazel flags that moved upstream: `use_typedshmd` now lives in `score_communication`, and the
  tracing stub is selected in `score_baselibs` (`generic_trace_library:implementation`).
- The benchmark starts the runtime with `InitializeRuntime(RuntimeConfiguration{Path{manifest}})`.
  The `(argc, argv)` overload it used is deprecated since 0.5.0 and its `score::StringLiteral`
  argument type no longer exists. The new call compiles against 0.1.1 and 0.6.0 alike.
- VS Code validates the manifest against `mw_com_config_schema.json` of communication 0.6.0 (the
  0.1.1 schema file no longer exists). The manifest itself needed no change.
- [docs/02](docs/02-mw-com-api-cheatsheet.md) is checked against 0.6.0 and has a new section,
  *What changed since 0.1.1*.

### Fixed

- Linking on Ubuntu 26.10: `rules_cc` 0.2.25 wraps every archive in `--start-lib`/`--end-lib`, and
  GNU ld 2.47 crashes with a segmentation fault on them. `--config=host` now sets
  `--features=-supports_start_end_lib`.

### Removed

- The `<algorithm>` force-include for GCC 15: `score_baselibs` 0.2.17 builds without it.
- `--@score_baselibs//score/json:base_library=nlohmann`: deprecated in baselibs 0.2.17 and without
  effect (vaJson is the only parser).

### Measured

| `BM_PingPong_NoFill`, median round trip, 64 B – 10 MiB | v0.5.0-alpha | v0.10.0 |
|---|---|---|
| one process | 6.65–6.91 µs | 5.12–5.26 µs (−22 to −25 % in both passes) |
| two processes | 13.14–13.53 µs | 12.00–12.35 µs (one-way p50 −0.5 µs in both passes) |

Both versions stay flat, fitted O(1) with 1–2 % RMS. Tails and the large `FullEcho` payloads
across two processes changed in both directions between passes and are treated as noise. The
results page sections 1–6 still describe the first run at v0.5.0-alpha; section 7 is the
comparison.

## 2026-11-11

### Added

- [Results page](https://intilinux-eng.github.io/scrample-pingpong-bench/) on GitHub Pages: the
  first native Linux run (Ryzen 9 7940HS, pinned cores, normal scheduler) with charts and a
  commented walk-through, built from `site/` by `.github/workflows/pages.yml`.
- `tools/summarise_results.py`: condenses Google Benchmark JSON into the page's data file and into
  Markdown tables for the results log.
- First entry in the [results log](docs/results-log.md).
- "Learning note" comments throughout `pingpong_bench.cpp`, with a suggested reading order.

### Fixed

- Build on GCC 15 (Ubuntu 25.10 and later) through an `<algorithm>` force-include.

## 2026-11-10

### Added

- The ping-pong benchmark for `score::mw::com`: Master and Reflector in one process or two,
  payloads from 64 B to 10 MiB, one-way latency percentiles, jitter and closed-loop throughput.
- `BM_PingPong_NoFill`, which separates the transfer from the cache effects of writing the payload.
- Knowledge base: [zero-copy IPC](docs/01-zero-copy-ipc.md), [mw::com API](docs/02-mw-com-api-cheatsheet.md),
  [benchmark design](docs/03-benchmark-design.md) and [Linux real-time tuning](docs/04-linux-realtime-tuning.md).
- `run_benchmark.sh` for pinned `SCHED_FIFO` runs, CI build and smoke test, dev container and
  WSL 2 instructions.

### Fixed

- Receive handlers are serialised: mw::com dispatches notifications on two worker threads, so a
  handler could overlap with itself and lose samples. The benchmark thread is woken only after
  `GetNewSamples()` returns.
- GCC's `maybe-uninitialized` false positive in optimised builds stays a warning.
