# Results log

Measured runs go here, newest first. A number without its context cannot be compared or trusted, so
every entry records the machine and the setup. Do not log runs from CI, WSL 2, containers or VMs as
performance results.

No runs have been recorded yet.

## How to add an entry

1. Prepare the machine as in [04 Linux real-time tuning](04-linux-realtime-tuning.md).
2. Run `src/benchmark/run_benchmark.sh` (and `BENCH_MODE=split` for the two-process topology).
3. Copy the template, fill it in from the JSON in `benchmark_results/`, and keep the JSON file
   name so the raw data can be found again.

## Template

```markdown
## YYYY-MM-DD, <short label>

| | |
|---|---|
| Commit | `<git rev-parse --short HEAD>` |
| Machine | <CPU model, cores, RAM> |
| Kernel | `<uname -r>`, PREEMPT_RT: yes / no |
| Boot parameters | `<isolcpus=... nohz_full=... rcu_nocbs=...>` |
| Governor | <performance / ...> |
| Topology | single process / two processes, cores <a>,<b>, SCHED_FIFO <prio> |
| cyclictest max | <µs, 60 s run on the same cores> |
| Raw data | `benchmark_results/<file>.json` |

### BM_PingPong (transfer only)

| Payload | one-way p50 (ns) | p99 (ns) | p99.9 (ns) | max (ns) | jitter stddev (ns) |
|---|---|---|---|---|---|
| 64 B | | | | | |
| 512 B | | | | | |
| 4 KiB | | | | | |
| 32 KiB | | | | | |
| 256 KiB | | | | | |
| 2 MiB | | | | | |
| 10 MiB | | | | | |

Fitted complexity: <BigO row>

### BM_PingPong_FullEcho (fill + full copy)

<same table>

### Observations

<What the numbers show, what was unexpected, what to try next.>
```
