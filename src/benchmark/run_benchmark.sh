#!/usr/bin/env bash
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
#
# Builds //src/benchmark:score_pingpong_bench with -c opt and runs it pinned to
# dedicated CPU cores under the SCHED_FIFO real-time policy.
#
# Usage:
#   src/benchmark/run_benchmark.sh [extra Google Benchmark flags...]
#
# Run it as your normal user. Only the benchmark itself is started through sudo
# (chrt -f needs root or CAP_SYS_NICE); the Bazel build never runs as root.
#
# Environment:
#   BENCH_CORES    "<master>,<reflector>" CPU ids            (default: 2,3)
#   BENCH_RT_PRIO  SCHED_FIFO priority, 1-99                 (default: 99)
#   BENCH_MODE     single = Master and Reflector in one process, pinned to both cores
#                  split  = two processes, one core each     (default: single)
#   BENCH_FORMAT   json | csv                                (default: json)
#   BENCH_OUT      result file  (default: benchmark_results/pingpong_<timestamp>.<format>)
#   BENCH_REPS     --benchmark_repetitions                   (default: 5)
#   BAZEL_CONFIG   Bazel --config, see .bazelrc              (default: host)
#
# For clean numbers, isolate the cores at boot, e.g.
#   isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3
# and select the "performance" cpufreq governor. The script warns if either is missing.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
readonly SCRIPT_DIR REPO_ROOT

readonly TARGET="//src/benchmark:score_pingpong_bench"
readonly BINARY="${REPO_ROOT}/bazel-bin/src/benchmark/score_pingpong_bench"
readonly MANIFEST="${SCRIPT_DIR}/etc/mw_com_config.json"

readonly CORES="${BENCH_CORES:-2,3}"
readonly RT_PRIO="${BENCH_RT_PRIO:-99}"
readonly MODE="${BENCH_MODE:-single}"
readonly FORMAT="${BENCH_FORMAT:-json}"
readonly REPS="${BENCH_REPS:-5}"
readonly BAZEL_CONFIG="${BAZEL_CONFIG:-host}"
readonly OUT="${BENCH_OUT:-${REPO_ROOT}/benchmark_results/pingpong_$(date +%Y%m%dT%H%M%S).${FORMAT}}"

die() {
    echo "error: $*" >&2
    exit 1
}

warn() {
    echo "warning: $*" >&2
}

# cpu_in_list <cpu> <list>, where <list> is a kernel CPU list such as "2-3,8".
cpu_in_list() {
    local cpu="$1" part low high
    local IFS=','
    for part in $2; do
        low="${part%-*}"
        high="${part#*-}"
        if ((cpu >= low && cpu <= high)); then
            return 0
        fi
    done
    return 1
}

# --- Validate the request -----------------------------------------------------

for tool in bazel taskset chrt; do
    command -v "${tool}" >/dev/null 2>&1 || die "'${tool}' not found in PATH"
done

[[ "${CORES}" =~ ^[0-9]+,[0-9]+$ ]] || die "BENCH_CORES must be '<master>,<reflector>', got '${CORES}'"
[[ "${RT_PRIO}" =~ ^[0-9]+$ ]] && ((RT_PRIO >= 1 && RT_PRIO <= 99)) || die "BENCH_RT_PRIO must be 1-99"
[[ "${MODE}" == "single" || "${MODE}" == "split" ]] || die "BENCH_MODE must be 'single' or 'split'"
[[ "${FORMAT}" == "json" || "${FORMAT}" == "csv" ]] || die "BENCH_FORMAT must be 'json' or 'csv'"

readonly MASTER_CORE="${CORES%,*}"
readonly REFLECTOR_CORE="${CORES#*,}"
[[ "${MASTER_CORE}" != "${REFLECTOR_CORE}" ]] || die "BENCH_CORES must name two different cores"

isolated=""
if [[ -r /sys/devices/system/cpu/isolated ]]; then
    isolated="$(</sys/devices/system/cpu/isolated)"
fi

for core in "${MASTER_CORE}" "${REFLECTOR_CORE}"; do
    [[ -d "/sys/devices/system/cpu/cpu${core}" ]] || die "CPU ${core} does not exist on this machine"

    if ! cpu_in_list "${core}" "${isolated}"; then
        warn "CPU ${core} is not isolated (isolcpus=); other tasks can preempt the benchmark"
    fi

    governor_file="/sys/devices/system/cpu/cpu${core}/cpufreq/scaling_governor"
    if [[ -r "${governor_file}" && "$(<"${governor_file}")" != "performance" ]]; then
        warn "CPU ${core} uses the '$(<"${governor_file}")' governor; 'performance' gives steadier latency"
    fi
done

# --- Build (as the invoking user) ---------------------------------------------

cd "${REPO_ROOT}"
echo ">> bazel build -c opt --config=${BAZEL_CONFIG} ${TARGET}"
bazel build -c opt --config="${BAZEL_CONFIG}" "${TARGET}"
[[ -x "${BINARY}" ]] || die "build succeeded but ${BINARY} is missing"

# --- Run ------------------------------------------------------------------------

# "env" is a no-op prefix that keeps the command arrays below uniform when already root.
privileged=(env)
if ((EUID != 0)); then
    command -v sudo >/dev/null 2>&1 || die "SCHED_FIFO needs root: run as root or install sudo"
    privileged=(sudo)
    sudo -v
fi

mkdir -p "$(dirname -- "${OUT}")"

common_args=("--service_instance_manifest=${MANIFEST}")
benchmark_args=(
    "--benchmark_out=${OUT}"
    "--benchmark_out_format=${FORMAT}"
    "--benchmark_repetitions=${REPS}"
    "$@"
)

reflector_pid=""
cleanup() {
    if [[ -n "${reflector_pid}" ]]; then
        "${privileged[@]}" kill -TERM "${reflector_pid}" 2>/dev/null || true
        wait "${reflector_pid}" 2>/dev/null || true
    fi
}
trap cleanup EXIT

status=0
if [[ "${MODE}" == "single" ]]; then
    echo ">> single process on CPUs ${CORES}, SCHED_FIFO ${RT_PRIO}"
    "${privileged[@]}" taskset -c "${CORES}" chrt -f "${RT_PRIO}" \
        "${BINARY}" --role=both "${common_args[@]}" "${benchmark_args[@]}" || status=$?
else
    echo ">> reflector on CPU ${REFLECTOR_CORE}, master on CPU ${MASTER_CORE}, SCHED_FIFO ${RT_PRIO}"
    "${privileged[@]}" taskset -c "${REFLECTOR_CORE}" chrt -f "${RT_PRIO}" \
        "${BINARY}" --role=reflector "${common_args[@]}" &
    reflector_pid=$!

    "${privileged[@]}" taskset -c "${MASTER_CORE}" chrt -f "${RT_PRIO}" \
        "${BINARY}" --role=master "${common_args[@]}" "${benchmark_args[@]}" || status=$?
fi

# The result file was written by root; hand it back to the invoking user.
if ((EUID != 0)) && [[ -e "${OUT}" ]]; then
    sudo chown "$(id -u):$(id -g)" "${OUT}"
fi

if ((status != 0)); then
    die "benchmark exited with status ${status}"
fi
echo ">> results written to ${OUT}"
