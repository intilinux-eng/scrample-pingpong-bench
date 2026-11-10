# 03 Benchmark design: decisions and the mistakes they avoid

Each section is one decision in `src/benchmark/pingpong_bench.cpp`, the reason for it, and what the
numbers would have meant without it.

## 1. One slot type sized for the largest payload

`mw::com` event types are fixed at compile time, so a payload cannot be resized per iteration.
The payload is a 10 MiB buffer with a `payload_size` field, and each run uses the first N bytes.

The alternative is one type per size, which needs one event (and one manifest entry) per size. It
measures the same thing, because the transport passes a slot reference in either case, and costs
seven times the configuration.

## 2. Two benchmarks, because "echo the same data" is not O(1)

The original plan was a single ping-pong where the Reflector sends the same data back. A received
sample cannot be re-published on another event, so sending it back means copying N bytes. That copy
would make the round trip linear in N and say nothing about the middleware.

- `BM_PingPong` times `Send(ping)` until the pong arrives. The Reflector echoes the header only.
- `BM_PingPong_FullEcho` times allocate, fill, send, until the pong arrives. The Reflector copies
  all N bytes.

The first is the measurement. The second is the control that shows what the first leaves out.

## 3. Where the timestamps are taken

- **Start:** immediately before `Send()` in `BM_PingPong`, after the payload has been written.
  Writing N bytes is the application's work, not the transport's.
- **End:** in the Master's receive handler, before it reads the sample and before the benchmark
  thread is woken through the condition variable. Waking the benchmark thread is harness overhead.

Still inside the measurement: the notification that triggers each receive handler, twice per round
trip. That is a real part of event-driven `mw::com` latency and belongs there.

## 4. `steady_clock`, not `high_resolution_clock`

In libstdc++ `high_resolution_clock` is an alias for `system_clock`, which NTP may adjust mid-run.
`steady_clock` is `CLOCK_MONOTONIC` and is read without a system call.

## 5. Manual timing in Google Benchmark

The benchmarks use `UseManualTime()` and report the measured round-trip time through
`SetIterationTime()`. The default would time the whole loop body, including the payload fill and the
bookkeeping. Per-iteration samples go into a vector that is reserved before the loop, so recording a
sample never allocates.

## 6. One-way latency is RTT / 2

That assumes both directions cost the same. It holds for `BM_PingPong`, where both messages are a
header-sized update of an identical slot type. In `BM_PingPong_FullEcho` the two legs differ, so the
value is an average, not the latency of either leg.

## 7. What "jitter" means here

Two numbers, both over the one-way latency samples of a run: the standard deviation, and peak to
peak (max minus min). The percentiles (p50, p99, p99.9) usually say more than either. On a machine
without core isolation the maximum is dominated by scheduler preemption, not by the middleware.

## 8. Throughput is closed-loop

One message is in flight at a time, so messages per second is 1 / RTT. This is the right figure for
request-reply traffic and the wrong one for streaming. A streaming test would send without waiting
and count what arrives.

## 9. Start-up handshake

A ping sent before the Reflector has subscribed is lost. The Master therefore repeats a small ping
until one comes back, and only then starts measuring. Both sides offer their service first and
connect second, so the start order of the two processes does not matter.

## 10. Memory locking only when it is safe

`mlockall(MCL_CURRENT | MCL_FUTURE)` keeps page faults out of the measurement. With a finite
`RLIMIT_MEMLOCK` it can succeed while the process is small and then make later shared-memory
mappings fail. The benchmark locks memory only as root or with an unlimited limit, and says so when
it does not.

## 11. Handlers take a lock, and the wake-up comes last

The first version did all its work in the receive handlers without a lock, on the assumption that
a handler for one event never runs twice at once. The first CI run disproved it: about half of the
benchmark runs ended with "timed out waiting for the pong", and the short ones (few iterations)
passed. That pattern, failure in proportion to the number of round trips, points to a race.

The cause is in the middleware's dispatch. `mw::com` hands notifications to a pool of two worker
threads. In a ping-pong the reply to message k+1 can arrive while the handler for message k is still
returning, so the second worker enters the same handler. Both then call `GetNewSamples()` on the
same proxy event, which is not thread-safe, and a sample goes missing.

Two changes fixed it:

- each handler holds a mutex for its whole body, so access to the proxy and skeleton is serialised;
- the Master's handler wakes the benchmark thread only after `GetNewSamples()` has returned. Waking
  it from inside the callback let the next ping go out while the handler was still reading.

The lesson for benchmarks in general: a harness that sends the next message the moment the previous
one is acknowledged is the worst case for this kind of overlap, and finds it quickly.

## Open questions and next experiments

- **Polling instead of receive handlers.** A busy-polling consumer skips the notification path and
  should be faster, at the price of a core. Worth a third benchmark.
- **More than one subscriber**, and ASIL-B instances with their extra control segment.
- **Histogram export** of the raw samples, not only the summary counters.
- **Cross-check against upstream.** The communication repository has its own
  `score/mw/com/performance_benchmarks`; comparing methods and results would validate both.
