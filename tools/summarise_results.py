#!/usr/bin/env python3
# *******************************************************************************
# Copyright (c) 2026 Contributors to the Eclipse Foundation
#
# See the NOTICE file(s) distributed with this work for additional
# information regarding copyright ownership.
#
# This program and the accompanying materials are made available under the
# terms of the Apache License Version 2.0 which is available at
# https://www.apache.org/licenses/LICENSE-2.0
#
# SPDX-License-Identifier: Apache-2.0
# *******************************************************************************
"""Condenses Google Benchmark JSON output into the data file of the results page.

Usage:
    tools/summarise_results.py --label "single process" run_single.json \
                               --label "two processes"  run_split.json \
                               --machine "..." --out site/data/results.json

For every benchmark and payload size it keeps the *median over repetitions* of each counter.
The median is used rather than the mean because one disturbed repetition (an interrupt storm, a
frequency change) would drag a mean along, while the median ignores it. The script also prints a
Markdown table per run, ready to paste into docs/results-log.md.
"""

import argparse
import json
import statistics
import sys
from pathlib import Path

COUNTERS = (
    "oneway_min_ns",
    "oneway_mean_ns",
    "oneway_p50_ns",
    "oneway_p99_ns",
    "oneway_p999_ns",
    "oneway_max_ns",
    "jitter_stddev_ns",
    "wall_round_trips_per_sec",
)


def size_label(n: int) -> str:
    if n >= 1 << 20:
        return f"{n / (1 << 20):g} MiB"
    if n >= 1 << 10:
        return f"{n / (1 << 10):g} KiB"
    return f"{n} B"


def summarise(path: Path) -> dict:
    raw = json.loads(path.read_text())
    per_bench: dict[str, dict[int, list[dict]]] = {}
    big_o: dict[str, dict] = {}

    medians: dict[str, dict[int, list[dict]]] = {}

    for entry in raw["benchmarks"]:
        family = entry["run_name"].split("/")[0]
        if entry.get("error_occurred"):
            raise SystemExit(f"{path}: {entry['name']} failed: {entry.get('error_message')}")
        if entry.get("run_type") == "iteration":
            size = int(entry["payload_bytes"])
            per_bench.setdefault(family, {}).setdefault(size, []).append(entry)
        elif entry.get("aggregate_name") == "median":
            # Present when the run used --benchmark_report_aggregates_only: then the per-repetition
            # rows are missing and Google Benchmark's own median is the best available value.
            size = int(entry["payload_bytes"])
            medians.setdefault(family, {}).setdefault(size, []).append(entry)
        elif entry.get("aggregate_name") == "BigO":
            big_o.setdefault(family, {})["big_o"] = entry["big_o"]
        elif entry.get("aggregate_name") == "RMS":
            big_o.setdefault(family, {})["rms"] = entry["rms"]

    for family, sizes in medians.items():
        per_bench.setdefault(family, sizes)

    benchmarks = {}
    for family, sizes in per_bench.items():
        rows = []
        for size in sorted(sizes):
            reps = sizes[size]
            repetitions = len(reps) if reps[0].get("run_type") == "iteration" else int(reps[0]["repetitions"])
            row = {"bytes": size, "label": size_label(size), "repetitions": repetitions}
            # Round-trip time as Google Benchmark reports it (manual time, microseconds here).
            row["rtt_us"] = statistics.median(r["real_time"] for r in reps)
            row["iterations"] = int(statistics.median(r["iterations"] for r in reps))
            for counter in COUNTERS:
                row[counter] = statistics.median(r[counter] for r in reps)
            rows.append(row)
        benchmarks[family] = {"rows": rows, **big_o.get(family, {})}

    # Only the context that matters for interpreting the numbers. host_name and the executable
    # path are dropped on purpose: the output is meant to be published.
    keep = ("date", "num_cpus", "mhz_per_cpu", "cpu_scaling_enabled", "load_avg", "library_version",
            "library_build_type", "topology", "transport", "slot_capacity_bytes", "caches")
    context = {key: raw["context"][key] for key in keep if key in raw["context"]}
    return {"context": context, "benchmarks": benchmarks}


def markdown(label: str, run: dict) -> str:
    out = [f"### {label}", ""]
    for family, data in run["benchmarks"].items():
        fit = data.get("big_o", "?")
        rms = data.get("rms")
        rms_text = f", RMS {rms * 100:.0f} %" if rms is not None else ""
        out += [
            f"**{family}**, fitted complexity {fit}{rms_text}",
            "",
            "| Payload | RTT (µs) | one-way p50 (ns) | p99 (ns) | p99.9 (ns) | max (ns) | jitter stddev (ns) |",
            "|---|---|---|---|---|---|---|",
        ]
        for r in data["rows"]:
            out.append(
                f"| {r['label']} | {r['rtt_us']:.1f} | {r['oneway_p50_ns']:.0f} | {r['oneway_p99_ns']:.0f} "
                f"| {r['oneway_p999_ns']:.0f} | {r['oneway_max_ns']:.0f} | {r['jitter_stddev_ns']:.0f} |"
            )
        out.append("")
    return "\n".join(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--label", action="append", required=True, help="name of the next run file")
    parser.add_argument("files", nargs="+", type=Path, help="Google Benchmark JSON files")
    parser.add_argument("--machine", default="", help="free-text machine description")
    parser.add_argument("--setup", default="", help="free-text description of the run setup")
    parser.add_argument("--commit", default="", help="git commit of the measured code")
    parser.add_argument("--date", default="", help="date of the run")
    parser.add_argument("--out", type=Path, required=True, help="output JSON for the page")
    parser.add_argument("--js-var", default="PINGPONG_RESULTS",
                        help="global name the .js copy assigns (one per data file on a page)")
    args = parser.parse_intermixed_args()  # allows "--label A a.json --label B b.json"

    if len(args.label) != len(args.files):
        parser.error("give exactly one --label per file")

    runs = []
    for label, path in zip(args.label, args.files):
        run = summarise(path)
        run["label"] = label
        run["source"] = path.name
        runs.append(run)
        print(markdown(label, run))

    page_data = {
        "date": args.date,
        "commit": args.commit,
        "machine": args.machine,
        "setup": args.setup,
        "runs": runs,
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(page_data, indent=1) + "\n")
    # The same data as a script, so the page renders synchronously and also works from file://.
    js_out = args.out.with_suffix(".js")
    js_out.write_text(f"window.{args.js_var} = " + json.dumps(page_data, indent=1) + ";\n")
    print(f"wrote {args.out} and {js_out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
