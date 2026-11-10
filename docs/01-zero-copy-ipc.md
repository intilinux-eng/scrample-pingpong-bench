# 01 Zero-copy IPC: what is O(1) and what is not

## The claim

Sending N bytes between two processes through a socket or a pipe costs at least two copies: user
space to kernel on the sender, kernel to user space on the receiver. Middleware that serializes adds
more. All of them touch every byte, so the cost grows linearly with N.

A zero-copy transport avoids this by changing what is sent. The producer writes the data straight
into memory that both processes have mapped. The transport then passes a **reference** to that
memory, a few bytes, whatever the size of the data behind it. The consumer reads the data where it
already is.

So the claim "zero-copy transfer is O(1)" is about the transport step only. It is worth being exact
about what it leaves out.

## What stays O(N)

| Work | Who pays | Scales with |
|---|---|---|
| Writing the payload into the slot | producer | N |
| Reading the payload | consumer | bytes actually read |
| First touch of each page (page fault) | whoever touches it first | N, once |
| Moving cache lines to the consumer's core | consumer, on read | bytes actually read |
| Copying into another message (an "echo") | the application | N |
| Cold caches after writing a large payload | whatever runs next | N, up to the cache size |

The cache effects are easy to overlook. Zero-copy removes the middleware's copies, but when the consumer
runs on another core, reading the data still pulls it across the cache hierarchy. A consumer that
reads all 10 MiB pays for 10 MiB. The saving is that it pays once instead of three or four times,
and pays nothing for bytes it never looks at.

## How score::mw::com does it

`score::mw::com` is the communication middleware of Eclipse S-CORE. Its API follows AUTOSAR Adaptive
`ara::com` (skeletons, proxies, events). Its shared-memory binding is known as LoLa. This is my
understanding of that binding at `score_communication` 0.1.1; the design documents in the
[communication repository](https://github.com/eclipse-score/communication) are the authority.

- **Slots.** For every event of a service instance the provider creates a fixed number of sample
  slots in a shared-memory segment (`numberOfSampleSlots` in the manifest). Each slot holds one
  object of the event's data type.
- **Publish.** `Allocate()` returns a free slot. The application fills it in place. `Send()` marks
  it as the newest sample. No payload byte moves.
- **Consume.** A subscriber gets a `SamplePtr` that points into the same segment. While the pointer
  is alive the slot is not reused, which is why a subscriber declares up front how many samples it
  may hold (`Subscribe(max_sample_count)`).
- **Notification.** A subscriber either polls with `GetNewSamples()` or registers a receive handler.
  The handler is triggered through a separate message-passing channel, so the latency of the handler
  path includes that notification.

## Consequences for the data type

The data type lives in shared memory and is read by another process, so:

- its size is fixed at compile time. There is no "send N bytes" call; a variable-size payload is a
  fixed-capacity buffer plus a length field;
- it must not contain pointers into the process's own heap: no `std::string`, no `std::vector` with
  the default allocator;
- a large type must never be created on the stack. The benchmark's payload is about 10 MiB and only
  ever exists inside slots returned by `Allocate()`.

## How the benchmark tests the claim

`BM_PingPong_NoFill` times only `Send()` until the reply arrives, for payload sizes from 64 B to
10 MiB, and lets Google Benchmark fit a complexity curve. If the transport is zero-copy, the curve
is flat. `BM_PingPong` does the same right after writing the payload, which exposes the cache cost
from the table above. `BM_PingPong_FullEcho` adds the producer's fill and a full copy on the other
side to the timed region, which shows the linear cost the first benchmark deliberately leaves out.
See [03 Benchmark design](03-benchmark-design.md), section 12 for what the three curves looked like.
