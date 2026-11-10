# 04 Linux real-time tuning for latency measurements

A latency benchmark measures the whole machine. Without preparation, the tail of the distribution
shows the scheduler, interrupts and frequency scaling, not the code under test. This is the
checklist `run_benchmark.sh` assumes, with a way to verify each item.

None of it applies to WSL 2, containers on a shared host, or VMs: there the hypervisor schedules the
"cores". Use those for functional runs only.

## Checklist

| # | Setting | Why | Verify |
|---|---|---|---|
| 1 | Isolate the benchmark cores | keeps other tasks off them | `cat /sys/devices/system/cpu/isolated` |
| 2 | Stop the timer tick on them | removes the periodic tick interrupt | `cat /sys/devices/system/cpu/nohz_full` |
| 3 | Move RCU callbacks away | removes kernel housekeeping | kernel command line |
| 4 | Keep device interrupts away | an IRQ preempts even `SCHED_FIFO` 99 | `/proc/interrupts` |
| 5 | `performance` governor | no frequency ramp-up inside a sample | `cpupower frequency-info` |
| 6 | Pin the process | no migration, warm caches | `taskset -cp <pid>` |
| 7 | `SCHED_FIFO` | not preempted by normal tasks | `chrt -p <pid>` |
| 8 | Lock memory | no page faults mid-measurement | `grep VmLck /proc/<pid>/status` |

## Boot parameters (items 1 to 4)

For cores 2 and 3, add to the kernel command line (`GRUB_CMDLINE_LINUX` in `/etc/default/grub`,
then `sudo update-grub` and reboot):

```text
isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3 irqaffinity=0-1
```

- `isolcpus` removes the cores from the general scheduler. Only tasks pinned there run there.
- `nohz_full` stops the scheduler tick on a core while it runs a single task.
- `rcu_nocbs` moves RCU callback processing to other cores.
- `irqaffinity` sets the default CPU set for device interrupts.

Pick two physical cores. If hyper-threading is on, the sibling of an isolated core shares its
execution resources; isolate the sibling too or disable SMT. `lscpu -e` shows which CPUs share a core.

## Frequency scaling (item 5)

```bash
sudo cpupower frequency-set -g performance
```

`run_benchmark.sh` warns when a benchmark core uses another governor.

## Pinning and scheduling (items 6 and 7)

```bash
sudo taskset -c 2,3 chrt -f 99 ./score_pingpong_bench ...
```

- `taskset -c` sets the CPU affinity. Threads the process creates later inherit it.
- `chrt -f 99` selects `SCHED_FIFO` at the highest priority. It needs root or `CAP_SYS_NICE`.
  Threads inherit the policy, including the middleware's own threads.

Two things to know about `SCHED_FIFO`:

- A FIFO task that never blocks starves everything else on its core. The benchmark blocks between
  messages, so it is safe; a busy-polling variant would not be.
- The kernel throttles real-time tasks by default to 95 % of each second
  (`/proc/sys/kernel/sched_rt_runtime_us` = 950000). A task that spins gets paused for 50 ms per
  second, which appears as a huge latency spike. Writing `-1` disables the throttle; only do that
  on isolated cores.

In split mode the script pins the Master and the Reflector to one core each. In single-process mode
both cores are given to the process and the scheduler places the threads.

## Memory (item 8)

The benchmark calls `mlockall(MCL_CURRENT | MCL_FUTURE)` when started as root. That maps and pins
every page up front, including the shared-memory slots, so the first access to a 10 MiB payload
does not fault. The shared memory lives in `/dev/shm`; it needs about 100 MiB there, and Docker's
default of 64 MiB is too small (`--shm-size`, already set in the dev container).

## The kernel itself

A standard kernel can delay even a `SCHED_FIFO` task while it runs non-preemptible kernel code. A
`PREEMPT_RT` kernel bounds that delay. `uname -v` shows `PREEMPT_RT` when it is active.

Before trusting any result, measure the machine without the middleware:

```bash
sudo cyclictest --mlockall --priority=99 --affinity=2,3 --threads=2 --interval=200 --duration=60
```

`cyclictest` (package `rt-tests`) reports the wake-up latency of the system. If its maximum is
100 µs, no benchmark on that machine can show a meaningful p99.9 below that.
