# Changelog

What changed in this repository, newest first, and what is being worked on right now. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). There are no version tags; entries
are dated. Measured numbers live in the [results log](docs/results-log.md), not here.

## Work in progress

- **Measuring S-CORE v0.10.0 against v0.5.0-alpha.** A first A/B pass (both binaries alternated on
  the same machine, same settings) is summarised under [Unreleased](#unreleased). A second pass with
  the exact binary of this branch is running. Its results will go into the
  [results log](docs/results-log.md) and the [results page](https://intilinux-eng.github.io/scrample-pingpong-bench/),
  which still shows the run made at `score_communication` 0.1.1.
- **Finding where the single-process speed-up comes from.** The notification path reads the same in
  both versions (it moved from `NotifyEventHandler` to `MessagePassingServiceInstance`, same
  algorithm), so the next step is a `perf` profile of both binaries rather than more reading.
- **Upstream.** [eclipse-score/scrample](https://github.com/eclipse-score/scrample) still pins
  v0.5.0-alpha. Its update PRs stall (the repository has no CODEOWNERS and no owning team in the
  S-CORE project plan). The plan is to ask on the S-CORE channels whether scrample is still
  maintained before offering the fixes found here.

## Unreleased

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

### Measured (first A/B pass, provisional)

Same laptop as the 2026-11-11 run, cores 2 and 3 pinned with `taskset`, normal scheduler,
performance governor, 5 repetitions, versions alternated. Medians over the repetitions.

| | v0.5.0-alpha (comm 0.1.1) | v0.10.0 (comm 0.6.0) |
|---|---|---|
| `BM_PingPong_NoFill`, one process, round trip | 6.6–6.8 µs | 5.1 µs (−23 %, every payload size) |
| `BM_PingPong_NoFill`, two processes, round trip | 13.2–15.7 µs | 12.6–13.1 µs |
| one-way p50, two processes | 6.4–6.7 µs | 6.1–6.2 µs |

Both versions stay flat from 64 B to 10 MiB. Two-process tails (p99.9) and `BM_PingPong_FullEcho`
at 2–10 MiB across two processes moved in both directions between runs; the second pass decides
whether those differences are real.

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
