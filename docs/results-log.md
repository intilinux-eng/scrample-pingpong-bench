# Results log

Measured runs go here, newest first. A number without its context cannot be compared or trusted, so
every entry records the machine and the setup. Do not log runs from CI, WSL 2, containers or VMs as
performance results.

## 2026-11-11, Ryzen laptop, pinned cores, normal scheduler (learning run)

Not a real-time result: no `SCHED_FIFO`, no isolated cores, memory not locked, desktop session
running. The shapes of the curves are reliable; the tail values (p99.9, max) are not. The
commented charts are on the [results page](https://intilinux-eng.github.io/scrample-pingpong-bench/).

| | |
|---|---|
| Commit | `4f2c959` |
| Machine | AMD Ryzen 9 7940HS, 8 cores / 16 threads (Zen 4), 1 MiB L2 per core, 16 MiB L3, 28 GiB RAM |
| Kernel | `7.3.0-9-generic` (Ubuntu 26.10), PREEMPT_RT: no (PREEMPT_DYNAMIC) |
| Compiler | GCC 15.3, `-c opt` |
| Boot parameters | none (`quiet splash`) |
| Governor | powersave (amd-pstate, EPP `balance_performance`) |
| Topology | single process on cores 2,3 / two processes, Master core 2, Reflector core 3; `SCHED_OTHER` via `taskset`, no `chrt` |
| cyclictest max | not measured |
| Repetitions | 5; every value below is the median over the repetitions |
| Raw data | `benchmark_results/linux_single.json`, `benchmark_results/linux_split.json` (local, untracked); condensed in [`site/data/results.json`](../site/data/results.json) |

### Observations

- **Zero-copy holds.** `BM_PingPong_NoFill` is flat from 64 B to 10 MiB: 6.6 µs in one process,
  13.5 µs in two, fitted O(1) with 1 % RMS in both.
- **The fill outside the timer still costs.** `BM_PingPong` rises to 13.8 µs (single) and 31.9 µs
  (two processes) at 10 MiB. Writing 10 MiB evicts the L2/L3 working set of the next round trip.
  Its O(log N) fit has an RMS of about 180 %, so it does not describe the data.
- **FullEcho falls off the L3 cliff.** 2 MiB → 10 MiB is 5× the data but 8× the time (129 µs →
  1048 µs): about 50 GB/s effective while the buffers fit in L3, about 30 GB/s once they go to DRAM.
- **The process boundary costs about 7 µs per round trip.** In one process mw::com calls local
  receive handlers directly (`NotifyEventLocally`); across processes the notification goes through a
  POSIX message queue. A control run with two cores per process (Master 2,3, Reflector 4,5;
  NoFill only, 3 repetitions) gave 14.0 µs at 64 B and 14.6 µs at 10 MiB, so core sharing is not the
  cause.
- **Long tail from the normal scheduler.** NoFill one-way p50 is about 3.2 µs, p99.9 about 7 µs, but
  the max per run ranges from about 30 µs to 1.4 ms. At 4 KiB the single-process max (151 µs) is
  higher than the two-process max (106 µs): a max is one sample and mostly measures disturbances.
- **Next:** repeat with `isolcpus`, `SCHED_FIFO` (`run_benchmark.sh`), the performance governor and
  a `cyclictest` baseline, and compare the tail.

#### Single process (cores 2,3)

**BM_PingPong**, fitted complexity lgN, RMS 183 %

| Payload | RTT (µs) | one-way p50 (ns) | p99 (ns) | p99.9 (ns) | max (ns) | jitter stddev (ns) |
|---|---|---|---|---|---|---|
| 64 B | 6.5 | 3200 | 4108 | 6677 | 89957 | 710 |
| 512 B | 6.4 | 3176 | 4058 | 6261 | 76177 | 446 |
| 4 KiB | 6.5 | 3181 | 4262 | 7914 | 71344 | 723 |
| 32 KiB | 6.6 | 3200 | 4433 | 30204 | 103132 | 1506 |
| 256 KiB | 6.6 | 3240 | 4874 | 10960 | 94386 | 1133 |
| 2 MiB | 8.0 | 4117 | 7774 | 24840 | 646842 | 3477 |
| 10 MiB | 13.8 | 5440 | 16074 | 30525 | 820345 | 7103 |

**BM_PingPong_NoFill**, fitted complexity (1), RMS 1 %

| Payload | RTT (µs) | one-way p50 (ns) | p99 (ns) | p99.9 (ns) | max (ns) | jitter stddev (ns) |
|---|---|---|---|---|---|---|
| 64 B | 6.7 | 3241 | 4738 | 7608 | 196430 | 1022 |
| 512 B | 6.7 | 3251 | 4969 | 9828 | 1381843 | 5692 |
| 4 KiB | 6.6 | 3230 | 4513 | 6922 | 151303 | 553 |
| 32 KiB | 6.7 | 3236 | 4869 | 7689 | 105906 | 501 |
| 256 KiB | 6.6 | 3231 | 4598 | 7078 | 171214 | 656 |
| 2 MiB | 6.5 | 3236 | 4474 | 6988 | 30700 | 300 |
| 10 MiB | 6.6 | 3246 | 4508 | 7003 | 87222 | 383 |

**BM_PingPong_FullEcho**, fitted complexity NlgN, RMS 17 %

| Payload | RTT (µs) | one-way p50 (ns) | p99 (ns) | p99.9 (ns) | max (ns) | jitter stddev (ns) |
|---|---|---|---|---|---|---|
| 64 B | 7.3 | 3536 | 5234 | 8140 | 313440 | 1308 |
| 512 B | 7.3 | 3542 | 5330 | 9062 | 1078400 | 3610 |
| 4 KiB | 7.4 | 3656 | 5254 | 8410 | 442494 | 1809 |
| 32 KiB | 9.3 | 4463 | 7033 | 10529 | 111682 | 888 |
| 256 KiB | 16.9 | 8526 | 15048 | 21624 | 488406 | 3263 |
| 2 MiB | 128.8 | 61030 | 111126 | 139282 | 489658 | 16217 |
| 10 MiB | 1047.7 | 513716 | 732792 | 864280 | 864280 | 67798 |

#### Two processes (Master core 2, Reflector core 3)

**BM_PingPong**, fitted complexity lgN, RMS 180 %

| Payload | RTT (µs) | one-way p50 (ns) | p99 (ns) | p99.9 (ns) | max (ns) | jitter stddev (ns) |
|---|---|---|---|---|---|---|
| 64 B | 14.2 | 6702 | 10374 | 36506 | 1208014 | 10574 |
| 512 B | 13.5 | 6557 | 8991 | 13484 | 363486 | 2414 |
| 4 KiB | 13.9 | 6577 | 9852 | 27159 | 827012 | 6284 |
| 32 KiB | 13.6 | 6547 | 9226 | 13364 | 1021042 | 5830 |
| 256 KiB | 14.0 | 6662 | 9638 | 29152 | 1310514 | 10943 |
| 2 MiB | 15.6 | 7248 | 13178 | 42888 | 1022524 | 10119 |
| 10 MiB | 31.9 | 12818 | 113735 | 618632 | 1348553 | 37163 |

**BM_PingPong_NoFill**, fitted complexity (1), RMS 1 %

| Payload | RTT (µs) | one-way p50 (ns) | p99 (ns) | p99.9 (ns) | max (ns) | jitter stddev (ns) |
|---|---|---|---|---|---|---|
| 64 B | 13.6 | 6562 | 9192 | 12908 | 467554 | 2962 |
| 512 B | 13.7 | 6556 | 9482 | 17126 | 941208 | 5841 |
| 4 KiB | 13.3 | 6527 | 8766 | 11476 | 106076 | 986 |
| 32 KiB | 13.4 | 6532 | 8946 | 12117 | 135128 | 820 |
| 256 KiB | 13.5 | 6637 | 8766 | 13234 | 182760 | 1216 |
| 2 MiB | 13.7 | 6537 | 9307 | 22095 | 803865 | 5871 |
| 10 MiB | 13.3 | 6502 | 8826 | 12137 | 222726 | 1503 |

**BM_PingPong_FullEcho**, fitted complexity NlgN, RMS 16 %

| Payload | RTT (µs) | one-way p50 (ns) | p99 (ns) | p99.9 (ns) | max (ns) | jitter stddev (ns) |
|---|---|---|---|---|---|---|
| 64 B | 14.0 | 6792 | 9497 | 23202 | 874728 | 4811 |
| 512 B | 13.9 | 6822 | 9277 | 13199 | 163350 | 899 |
| 4 KiB | 14.3 | 7018 | 9578 | 15443 | 407255 | 3730 |
| 32 KiB | 17.2 | 8386 | 12623 | 19996 | 901401 | 4681 |
| 256 KiB | 22.0 | 10469 | 18964 | 29172 | 784175 | 5985 |
| 2 MiB | 135.9 | 62012 | 162293 | 314662 | 622488 | 21710 |
| 10 MiB | 1017.2 | 495424 | 722308 | 1203841 | 1203841 | 55703 |

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
