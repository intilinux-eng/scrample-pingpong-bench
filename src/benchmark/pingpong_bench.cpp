/********************************************************************************
 * Copyright (c) 2026 Contributors to the Eclipse Foundation
 *
 * See the NOTICE file(s) distributed with this work for additional
 * information regarding copyright ownership.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Apache License Version 2.0 which is available at
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * SPDX-License-Identifier: Apache-2.0
 ********************************************************************************/

// Ping-pong latency / jitter / throughput benchmark for score::mw::com (LoLa shared-memory binding).
//
//      Master  -- ping on Topic_A -->  Reflector
//      Master <-- pong on Topic_B --   Reflector
//
// The Master runs on the Google Benchmark thread. The Reflector has no thread of its own: it runs
// entirely inside the mw::com receive handler of Topic_A, on mw::com's worker threads. Both can
// live in one process
// (--role=both, the default) or in two (--role=master / --role=reflector), which is the
// configuration that actually crosses a process boundary.
//
// Three benchmarks are registered, because "echo the same data back" and "O(1)" cannot both hold:
// mw::com cannot re-publish a received sample on another event, so echoing the bytes costs a memcpy.
//
//   BM_PingPong           Timed region: Send(ping) -> pong received. The Master writes N payload
//                         bytes before the timed region, the Reflector echoes the header only.
//                         This is the transfer as an application sees it right after producing data.
//   BM_PingPong_NoFill    Same, but the payload bytes are not written at all. Writing megabytes
//                         evicts the CPU caches, which slows down whatever runs next. This variant
//                         removes that side effect and shows the transfer on its own.
//   BM_PingPong_FullEcho  Timed region: Allocate + fill N bytes + Send -> pong received. The
//                         Reflector memcpy()s all N bytes into its pong. This is the literal
//                         "exact same data back" flow and is O(N) by construction - the linear part
//                         is application memory bandwidth, not the middleware.
//
// API usage follows score_communication v0.1.1 as pinned by scrample's MODULE.bazel.
//
// ---------------------------------------------------------------------------------------------
// How to read this file (learning guide)
// ---------------------------------------------------------------------------------------------
// Comments that start with "Learning note" explain *why* a language feature or system call is
// used, for readers who are new to C++, IPC or benchmarking. The other comments document the
// design. Suggested reading order:
//
//   1. PingPongPayload       what travels through shared memory, and why its type is so strict
//   2. Topic*Interface       how mw::com turns one C++ class into a sender and a receiver side
//   3. Reflector             the side that answers: subscribe, receive, allocate, send
//   4. Master::RoundTrip     the measured exchange: where the clock starts and stops
//   5. Master::OnPong        how a result crosses from an mw::com thread to the benchmark thread
//   6. RunPingPong           how Google Benchmark is told what to measure
//   7. main()                start-up order, real-time preparations, shutdown
//
// Vocabulary used throughout:
//   Skeleton  the providing side of a service (it "offers" and sends events). ara::com term.
//   Proxy     the consuming side (it "finds" the service and subscribes to events).
//   Event     a typed, one-way data channel of a service: here "ping" and "pong".
//   Sample    one value sent on an event. In LoLa it is a slot in a shared-memory pool.
//   RTT       round-trip time: ping out and pong back. One-way latency is estimated as RTT / 2.

// Learning note: the headers below fall into three groups.
//   - Google Benchmark: the measurement harness (iterations, statistics, JSON/CSV output).
//   - score/mw/com: the middleware under test.
//   - POSIX (<sys/mman.h>, <sys/resource.h>, <unistd.h>): mlockall(), getrlimit(), geteuid(),
//     which the C++ standard library does not offer. They are used in LockMemory().
//   - The C++ standard library for everything else. <atomic>, <mutex> and <condition_variable>
//     are the thread-synchronisation tools; <chrono> is the clock.

#include <benchmark/benchmark.h>

#include "score/mw/com/runtime.h"
#include "score/mw/com/types.h"

#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

// Learning note: an unnamed namespace gives everything inside it internal linkage, the modern
// replacement for marking each function `static`. Nothing here can clash with a symbol of the
// same name in the S-CORE libraries, and the compiler knows the code is used only in this file.
namespace
{

// Learning note: this enables the literals 30s, 50ms, 2s used below. They produce typed
// std::chrono durations, so the unit is part of the type and "2" can never be misread as
// milliseconds when seconds were meant.
using namespace std::chrono_literals;

// steady_clock is CLOCK_MONOTONIC on Linux. high_resolution_clock is deliberately not used: in
// libstdc++ it aliases system_clock, which can jump under NTP corrections.
using Clock = std::chrono::steady_clock;
// Learning note: steady_clock::now() costs about 20 ns on x86-64 Linux, because it reads the TSC
// through the vDSO without a system call. That is small next to a ~10 µs round trip, but it is
// one reason the clock is read only twice per iteration (send and receive).

// Learning note: `constexpr` values are known at compile time. kMaxPayloadBytes in particular has
// to be, because it sizes the std::array inside the payload type, and a type's size is fixed
// when the program is compiled. The `{}` braces (list initialisation) reject narrowing
// conversions, and the U suffix keeps the arithmetic unsigned. 10U << 20U is 10 * 2^20 = 10 MiB.
constexpr std::size_t kMinPayloadBytes{64U};
constexpr std::size_t kMaxPayloadBytes{10U << 20U};

// Samples a subscriber may hold at once. Must stay below numberOfSampleSlots in mw_com_config.json.
//
// Learning note: the shared-memory pool of each event has a fixed number of slots (5 in the
// manifest). Every slot is either being written by the sender, published, or held by a reader.
// If readers could hold all of them, the sender's Allocate() would find no free slot and fail.
// Keeping the per-subscriber limit (2) below the pool size leaves the sender room to work.
constexpr std::size_t kMaxSamplesPerSubscription{2U};

constexpr auto kDiscoveryTimeout{30s};
constexpr auto kDiscoveryPollInterval{50ms};
constexpr auto kHandshakeAttemptTimeout{250ms};
constexpr auto kRoundTripTimeout{2s};

// Must match "instanceSpecifier" in etc/mw_com_config.json.
constexpr std::string_view kTopicASpecifier{"score/benchmark/topic_a"};
constexpr std::string_view kTopicBSpecifier{"score/benchmark/topic_b"};

// ---------------------------------------------------------------------------------------------
// Payload
// ---------------------------------------------------------------------------------------------

enum class EchoMode : std::uint32_t
{
    kHeaderOnly = 0U,
    kFullPayload = 1U,
};

// mw::com event types are fixed at compile time and live directly in shared memory, so a payload
// cannot be resized per iteration. The slot is therefore sized for the largest payload and
// `payload_size` says how many bytes of `data` an iteration uses. The middleware transfers the slot
// by reference whatever its size, which is exactly the property under test.
//
// At ~10 MiB this type must never be placed on the stack. It only ever exists inside slots handed
// out by SkeletonEvent::Allocate().
struct PingPongPayload
{
    std::uint64_t sequence_id;
    std::uint64_t payload_size;
    EchoMode echo_mode;
    std::uint32_t reserved;
    std::array<std::uint8_t, kMaxPayloadBytes> data;
};

// Learning note: why std::array and not std::vector or std::string? A std::vector stores a
// pointer to heap memory owned by the process that created it. Another process maps the shared
// memory at a different address and has no access to that heap, so the pointer would be
// meaningless or dangerous there. std::array keeps its bytes inline, inside the struct, so the
// whole payload is one contiguous block that is valid wherever it is mapped.
//
// The two static_asserts turn that rule into a compile error instead of a runtime crash:
//   - trivially copyable: no user-written copy/move/destructor logic, so the object can be
//     treated as raw bytes (what memcpy and shared memory do);
//   - standard layout: the member order and padding follow fixed rules, so two processes
//     compiled from this file agree on where every field lives.
// The fields are also ordered from largest to smallest alignment (8, 8, 4, 4, then bytes), which
// avoids hidden padding between them.
static_assert(std::is_trivially_copyable_v<PingPongPayload>, "payload is shared between processes as raw memory");
static_assert(std::is_standard_layout_v<PingPongPayload>, "payload layout must be identical in both processes");

// ---------------------------------------------------------------------------------------------
// Service interfaces (mw::com's code-first equivalent of an ara::com IDL)
// ---------------------------------------------------------------------------------------------
//
// Learning note: the "trait" pattern. Each interface is written once, as a class template over a
// `Trait`. mw::com supplies two traits:
//   - AsSkeleton<...> instantiates it with SkeletonTrait: Event<T> becomes a SkeletonEvent<T>,
//     which has Allocate() and Send();
//   - AsProxy<...> instantiates it with ProxyTrait: Event<T> becomes a ProxyEvent<T>, which has
//     Subscribe(), SetReceiveHandler() and GetNewSamples().
// So the sender and the receiver are generated from one definition and cannot disagree on the
// event names or types. In ara::com the same thing is generated from an ARXML/IDL file instead.
//
// `typename` and `template` in `typename Trait::template Event<...>` are required by C++ because
// `Trait` is a template parameter: without them the compiler cannot know that Event is a type
// template rather than a value. `using Trait::Base::Base;` inherits the base class constructors.
// The string "ping" must match "eventName" in etc/mw_com_config.json.

template <typename Trait>
class TopicAInterface : public Trait::Base
{
  public:
    using Trait::Base::Base;

    typename Trait::template Event<PingPongPayload> ping_{*this, "ping"};
};

template <typename Trait>
class TopicBInterface : public Trait::Base
{
  public:
    using Trait::Base::Base;

    typename Trait::template Event<PingPongPayload> pong_{*this, "pong"};
};

using TopicASkeleton = score::mw::com::AsSkeleton<TopicAInterface>;
using TopicAProxy = score::mw::com::AsProxy<TopicAInterface>;
using TopicBSkeleton = score::mw::com::AsSkeleton<TopicBInterface>;
using TopicBProxy = score::mw::com::AsProxy<TopicBInterface>;

// ---------------------------------------------------------------------------------------------
// Process-wide state
// ---------------------------------------------------------------------------------------------

// Learning note: a signal handler may interrupt the program between any two instructions, so it
// is allowed to do very little. Locking a mutex or printing from it can deadlock. Writing a
// lock-free std::atomic is one of the few safe actions, so the handler only sets a flag and the
// loops below check it. memory_order_relaxed is enough because the flag carries no other data
// that would need to become visible together with it.
std::atomic<bool> g_stop_requested{false};
std::atomic<bool> g_benchmark_failed{false};

void HandleSignal(int /*signal*/)
{
    g_stop_requested.store(true, std::memory_order_relaxed);
}

bool StopRequested() noexcept
{
    return g_stop_requested.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------------
// mw::com helpers
// ---------------------------------------------------------------------------------------------
//
// Learning note: error handling without exceptions. mw::com functions return a score::Result<T>
// (similar to C++23 std::expected): it holds either a value or an error code, checked with
// has_value() and read with value() or error(). Safety-related automotive code usually avoids
// exceptions because their cost and control flow are hard to bound and to certify, so every
// failure path here is an explicit `if`.
//
// Learning note: an "instance specifier" is a logical name ("score/benchmark/topic_a"). The
// manifest (mw_com_config.json) maps it to a concrete service type, instance id and binding (SHM).
// The code never names shared-memory files or ids, so deployment can change without recompiling.

template <typename SkeletonType>
std::unique_ptr<SkeletonType> CreateAndOffer(const std::string_view specifier_name)
{
    auto specifier = score::mw::com::InstanceSpecifier::Create(std::string{specifier_name});
    if (!specifier.has_value())
    {
        std::cerr << "Invalid instance specifier '" << specifier_name << "'\n";
        return nullptr;
    }

    auto created = SkeletonType::Create(specifier.value());
    if (!created.has_value())
    {
        std::cerr << "Unable to create skeleton '" << specifier_name << "': " << created.error() << '\n';
        return nullptr;
    }

    // Learning note: `std::move(created).value()` moves the skeleton out of the Result instead of
    // copying it. Skeletons own shared-memory resources and cannot be copied. The unique_ptr lets
    // the owner create the skeleton later than its own constructor (in Offer()), use nullptr for
    // "not created", and destroy it at a well-defined moment.
    auto skeleton = std::make_unique<SkeletonType>(std::move(created).value());
    // OfferService() creates the shared-memory pools and announces the service so that proxies,
    // in this process or another one, can find it.
    const auto offered = skeleton->OfferService();
    if (!offered.has_value())
    {
        std::cerr << "Unable to offer '" << specifier_name << "': " << offered.error() << '\n';
        return nullptr;
    }
    return skeleton;
}

// Polls service discovery until the peer has offered the service, a signal arrives, or the
// discovery timeout expires.
template <typename ProxyType>
std::unique_ptr<ProxyType> FindAndCreate(const std::string_view specifier_name)
{
    auto specifier = score::mw::com::InstanceSpecifier::Create(std::string{specifier_name});
    if (!specifier.has_value())
    {
        std::cerr << "Invalid instance specifier '" << specifier_name << "'\n";
        return nullptr;
    }

    const auto deadline = Clock::now() + kDiscoveryTimeout;
    while (!StopRequested() && Clock::now() < deadline)
    {
        auto handles = ProxyType::FindService(specifier.value());
        if (handles.has_value() && !handles.value().empty())
        {
            auto created = ProxyType::Create(handles.value().front());
            if (!created.has_value())
            {
                std::cerr << "Unable to create proxy '" << specifier_name << "': " << created.error() << '\n';
                return nullptr;
            }
            return std::make_unique<ProxyType>(std::move(created).value());
        }
        std::this_thread::sleep_for(kDiscoveryPollInterval);
    }

    std::cerr << "Service '" << specifier_name << "' was not found. Is the peer process running?\n";
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// Reflector: subscribes to Topic_A and answers every ping with a pong on Topic_B.
// ---------------------------------------------------------------------------------------------

// Learning note: `final` forbids deriving from the class. Copy and move are deleted because the
// receive handler captures `this`: if the object could be copied or moved, mw::com would still
// call the handler with the old address. Deleting them makes that mistake impossible to compile.
// This is the "rule of five": once one of the special members matters, decide all five.
class Reflector final
{
  public:
    Reflector() = default;
    Reflector(const Reflector&) = delete;
    Reflector& operator=(const Reflector&) = delete;
    Reflector(Reflector&&) = delete;
    Reflector& operator=(Reflector&&) = delete;

    ~Reflector()
    {
        if (ping_proxy_ != nullptr)
        {
            (void)ping_proxy_->ping_.UnsetReceiveHandler();
            ping_proxy_->ping_.Unsubscribe();
        }
        if (pong_skeleton_ != nullptr)
        {
            pong_skeleton_->StopOfferService();
        }
    }

    bool Offer()
    {
        pong_skeleton_ = CreateAndOffer<TopicBSkeleton>(kTopicBSpecifier);
        return pong_skeleton_ != nullptr;
    }

    bool Connect()
    {
        ping_proxy_ = FindAndCreate<TopicAProxy>(kTopicASpecifier);
        if (ping_proxy_ == nullptr)
        {
            return false;
        }

        // The handler is registered before subscribing so that no notification is missed.
        //
        // Learning note: event-driven reception. Instead of a thread that polls in a loop,
        // mw::com calls the handler when a new sample is published. The handler takes no
        // arguments; it only says "something arrived", and GetNewSamples() fetches the data.
        // The lambda is `noexcept` because an exception escaping into mw::com's worker thread
        // would terminate the program.
        auto& ping_event = ping_proxy_->ping_;
        if (!ping_event.SetReceiveHandler([this]() noexcept { OnPing(); }).has_value())
        {
            std::cerr << "Reflector: unable to set the receive handler on Topic_A\n";
            return false;
        }
        if (!ping_event.Subscribe(kMaxSamplesPerSubscription).has_value())
        {
            std::cerr << "Reflector: unable to subscribe to Topic_A\n";
            return false;
        }
        return true;
    }

    std::uint64_t FailedEchoes() const noexcept
    {
        return failed_echoes_.load(std::memory_order_relaxed);
    }

  private:
    // mw::com dispatches receive handlers on a pool of two worker threads, so a second invocation
    // can start while the first is still returning. Proxy events and skeleton events are not safe
    // for concurrent use, hence the lock around everything the handler touches.
    void OnPing() noexcept
    {
        const std::lock_guard<std::mutex> handler_lock{handler_mutex_};
        // Learning note: this is where zero-copy shows. SamplePtr is a read-only smart pointer
        // straight into the sender's shared-memory slot: no bytes were copied to deliver it,
        // whatever the payload size. While the SamplePtr exists the slot stays reserved for
        // this reader; when it is destroyed (at the end of the lambda) the slot is released.
        // `(void)` marks that the returned Result is ignored on purpose.
        (void)ping_proxy_->ping_.GetNewSamples(
            [this](score::mw::com::SamplePtr<PingPongPayload> ping) noexcept { Echo(*ping); },
            kMaxSamplesPerSubscription);
    }

    void Echo(const PingPongPayload& ping) noexcept
    {
        // Learning note: Allocate() reserves a free slot in Topic_B's shared-memory pool and
        // returns a writable pointer to it (SampleAllocateePtr). The pong is built in place, in
        // the memory the Master will read, so Send() later does not copy it either.
        auto allocation = pong_skeleton_->pong_.Allocate();
        if (!allocation.has_value())
        {
            failed_echoes_.fetch_add(1U, std::memory_order_relaxed);
            return;
        }

        auto pong = std::move(allocation).value();
        pong->sequence_id = ping.sequence_id;
        pong->payload_size = ping.payload_size;
        pong->echo_mode = ping.echo_mode;
        pong->reserved = 0U;
        if (ping.echo_mode == EchoMode::kFullPayload)
        {
            // payload_size comes from shared memory, so it is clamped before it is used as a length.
            const auto size = static_cast<std::size_t>(
                std::min<std::uint64_t>(ping.payload_size, std::uint64_t{kMaxPayloadBytes}));
            std::memcpy(pong->data.data(), ping.data.data(), size);
        }

        // Learning note: Send() takes the pointer by value, so std::move hands the slot over to
        // the middleware. After this line `pong` is empty and the sender can no longer write to a
        // sample that readers may already see. Ownership in the type system replaces a comment
        // saying "do not touch after sending".
        if (!pong_skeleton_->pong_.Send(std::move(pong)).has_value())
        {
            failed_echoes_.fetch_add(1U, std::memory_order_relaxed);
        }
    }

    // Declaration order matters: the proxy (and with it the handler that uses the skeleton) is
    // destroyed first.
    //
    // Learning note: C++ destroys members in the reverse order of their declaration. Declaring
    // the skeleton first means it is destroyed last, so a handler still running on the proxy can
    // never use a skeleton that is already gone.
    std::unique_ptr<TopicBSkeleton> pong_skeleton_;
    std::unique_ptr<TopicAProxy> ping_proxy_;
    std::mutex handler_mutex_;  // serialises OnPing(), see there
    std::atomic<std::uint64_t> failed_echoes_{0U};
};

// ---------------------------------------------------------------------------------------------
// Master: publishes pings on Topic_A and measures until the matching pong arrives on Topic_B.
// ---------------------------------------------------------------------------------------------

// Learning note: a plain struct with a nullable error message, not an exception or a
// std::string. RoundTrip() runs inside the measured loop, where allocating memory would add
// latency; string literals live in read-only memory and cost nothing to return.
struct RoundTripResult
{
    std::chrono::nanoseconds round_trip{0};
    const char* error{nullptr};  // nullptr on success
};

class Master final
{
  public:
    Master() = default;
    Master(const Master&) = delete;
    Master& operator=(const Master&) = delete;
    Master(Master&&) = delete;
    Master& operator=(Master&&) = delete;

    ~Master()
    {
        if (pong_proxy_ != nullptr)
        {
            (void)pong_proxy_->pong_.UnsetReceiveHandler();
            pong_proxy_->pong_.Unsubscribe();
        }
        if (ping_skeleton_ != nullptr)
        {
            ping_skeleton_->StopOfferService();
        }
    }

    bool Offer()
    {
        ping_skeleton_ = CreateAndOffer<TopicASkeleton>(kTopicASpecifier);
        return ping_skeleton_ != nullptr;
    }

    bool Connect()
    {
        pong_proxy_ = FindAndCreate<TopicBProxy>(kTopicBSpecifier);
        if (pong_proxy_ == nullptr)
        {
            return false;
        }

        auto& pong_event = pong_proxy_->pong_;
        if (!pong_event.SetReceiveHandler([this]() noexcept { OnPong(); }).has_value())
        {
            std::cerr << "Master: unable to set the receive handler on Topic_B\n";
            return false;
        }
        if (!pong_event.Subscribe(kMaxSamplesPerSubscription).has_value())
        {
            std::cerr << "Master: unable to subscribe to Topic_B\n";
            return false;
        }
        return true;
    }

    // Pings sent before the Reflector has finished subscribing are lost, so the loop is retried
    // until one full round trip succeeds.
    bool Handshake()
    {
        const auto deadline = Clock::now() + kDiscoveryTimeout;
        while (!StopRequested() && Clock::now() < deadline)
        {
            if (RoundTrip(kMinPayloadBytes, EchoMode::kHeaderOnly, true, kHandshakeAttemptTimeout).error == nullptr)
            {
                return true;
            }
        }
        std::cerr << "Master: no pong received during the handshake. Is the reflector running?\n";
        return false;
    }

    // One ping-pong exchange. Called from the benchmark thread only.
    // Precondition: payload_bytes <= kMaxPayloadBytes.
    RoundTripResult RoundTrip(const std::size_t payload_bytes,
                              const EchoMode mode,
                              const bool fill_payload,
                              const std::chrono::nanoseconds timeout) noexcept
    {
        // Learning note: a fresh sequence id per ping lets the Master match each pong to its own
        // ping. A late pong from an earlier, timed-out iteration therefore cannot end the wait.
        // The fill byte changes with the id, so stale data from a reused slot is also detected.
        const auto sequence_id = ++last_sequence_id_;
        const auto fill = static_cast<std::uint8_t>(sequence_id);

        const auto allocate_time = Clock::now();
        auto allocation = ping_skeleton_->ping_.Allocate();
        if (!allocation.has_value())
        {
            return {0ns, "Allocate() failed on Topic_A (no free sample slot)"};
        }

        auto ping = std::move(allocation).value();
        ping->sequence_id = sequence_id;
        ping->payload_size = payload_bytes;
        ping->echo_mode = mode;
        ping->reserved = 0U;
        if (fill_payload)
        {
            // Learning note: this is the O(N) part an application always pays: producing the
            // data. At 10 MiB it is far bigger than the L2 cache (1 MiB per core on a Ryzen 7940HS),
            // so it also pushes other data out of the caches. That side effect is why
            // BM_PingPong_NoFill exists.
            std::memset(ping->data.data(), fill, payload_bytes);
        }

        // Learning note: the clock starts here, after the fill. Everything from this point to
        // the pong is middleware: publish, notify the reader, wake its thread, the Reflector's
        // work, and the same on the way back.
        const auto send_time = Clock::now();
        if (!ping_skeleton_->ping_.Send(std::move(ping)).has_value())
        {
            return {0ns, "Send() failed on Topic_A"};
        }

        // Learning note: the condition-variable pattern. wait_for() releases the mutex, sleeps
        // until notified (or until the timeout), then re-locks the mutex and checks the
        // predicate. The predicate is essential for two reasons:
        //   - spurious wake-ups: a condition variable may wake without a notify;
        //   - lost wake-ups: if the pong arrived before this thread started waiting, the
        //     predicate is already true and wait_for() returns at once instead of sleeping.
        // Sleeping instead of spinning keeps the CPU free, but costs a scheduler wake-up of a few
        // microseconds. That wake-up is NOT in the measurement: the receive timestamp was taken
        // in OnPong() before the notify.
        std::unique_lock<std::mutex> lock{pong_mutex_};
        const bool received = pong_received_.wait_for(lock, timeout, [this, sequence_id]() noexcept {
            return last_pong_.sequence_id == sequence_id;
        });
        if (!received)
        {
            return {0ns, "timed out waiting for the pong on Topic_B"};
        }
        if (!last_pong_.intact)
        {
            return {0ns, "pong payload does not match the ping payload"};
        }

        // The O(1) benchmark times the transfer only. The full-echo benchmark also times the
        // allocation and the O(N) fill, as the literal ping-pong flow prescribes.
        const auto start_time = (mode == EchoMode::kFullPayload) ? allocate_time : send_time;
        return {std::chrono::duration_cast<std::chrono::nanoseconds>(last_pong_.received_at - start_time), nullptr};
    }

  private:
    struct PongRecord
    {
        std::uint64_t sequence_id{0U};
        Clock::time_point received_at{};
        bool intact{true};
    };

    // Runs on an mw::com worker thread. The final timestamp is taken here, before the benchmark
    // thread is woken, so that the harness' own wake-up is not counted as IPC latency.
    //
    // Two rules keep this correct, given that mw::com may run two invocations at once:
    //  - handler_mutex_ serialises all access to the proxy event (it is not thread-safe);
    //  - the benchmark thread is woken only after GetNewSamples() has returned. Waking it from
    //    inside the callback would let the next ping go out while this invocation is still reading.
    void OnPong() noexcept
    {
        const std::lock_guard<std::mutex> handler_lock{handler_mutex_};
        const auto received_at = Clock::now();

        std::optional<PongRecord> newest{};
        (void)pong_proxy_->pong_.GetNewSamples(
            [&newest, received_at](score::mw::com::SamplePtr<PingPongPayload> pong) noexcept {
                if (!newest.has_value() || pong->sequence_id > newest->sequence_id)
                {
                    newest = PongRecord{pong->sequence_id, received_at, IsIntact(*pong)};
                }
            },
            kMaxSamplesPerSubscription);
        if (!newest.has_value())
        {
            return;  // an earlier invocation already consumed the sample
        }

        // Learning note: the extra braces form a scope, so the lock is released before
        // notify_one(). Notifying while holding the lock would let the woken thread run straight
        // into a locked mutex and sleep again ("hurry up and wait").
        {
            const std::lock_guard<std::mutex> lock{pong_mutex_};
            last_pong_ = *newest;
        }
        pong_received_.notify_one();
    }

    // Spot check of the first and last byte, so that the check itself stays O(1).
    static bool IsIntact(const PingPongPayload& pong) noexcept
    {
        if (pong.echo_mode != EchoMode::kFullPayload || pong.payload_size == 0U)
        {
            return true;
        }
        if (pong.payload_size > kMaxPayloadBytes)
        {
            return false;
        }
        const auto fill = static_cast<std::uint8_t>(pong.sequence_id);
        const auto last = static_cast<std::size_t>(pong.payload_size) - 1U;
        return pong.data[0U] == fill && pong.data[last] == fill;
    }

    std::unique_ptr<TopicASkeleton> ping_skeleton_;
    std::unique_ptr<TopicBProxy> pong_proxy_;

    std::uint64_t last_sequence_id_{0U};  // benchmark thread only

    std::mutex handler_mutex_;  // serialises OnPong(), see there. Lock order: handler_mutex_, then pong_mutex_.
    std::mutex pong_mutex_;
    std::condition_variable pong_received_;
    PongRecord last_pong_{};  // guarded by pong_mutex_
};

// Non-owning. Set by main() before the benchmarks run and cleared before the Master is destroyed.
Master* g_master{nullptr};

// ---------------------------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------------------------

struct LatencyStats
{
    double min{0.0};
    double mean{0.0};
    double p50{0.0};
    double p99{0.0};
    double p999{0.0};
    double max{0.0};
    double stddev{0.0};
};

// Learning note: why percentiles and not just the mean? Latency distributions are not
// symmetric: most samples sit near the minimum and a few are much slower (a page fault, an
// interrupt, a preemption). The mean hides those outliers; p99 and p99.9 show them. Real-time
// systems are judged by the worst case, so max and p99.9 matter more than the average.
// p50 (the median) is the "typical" value and is robust against outliers.
//
// The standard deviation below is the population form (divide by N), which is fine for the
// thousands of samples a run collects.
//
// Sorts `samples` in place. Precondition: !samples.empty().
LatencyStats Summarise(std::vector<double>& samples)
{
    std::sort(samples.begin(), samples.end());

    const auto count = samples.size();
    const auto percentile = [&samples, count](const double quantile) {
        // Nearest-rank method.
        const auto rank = static_cast<std::size_t>(std::ceil(quantile * static_cast<double>(count)));
        return samples[std::clamp<std::size_t>(rank, 1U, count) - 1U];
    };

    LatencyStats stats{};
    stats.min = samples.front();
    stats.max = samples.back();
    stats.mean = std::accumulate(samples.cbegin(), samples.cend(), 0.0) / static_cast<double>(count);
    stats.p50 = percentile(0.50);
    stats.p99 = percentile(0.99);
    stats.p999 = percentile(0.999);

    const auto squared_deviations =
        std::accumulate(samples.cbegin(), samples.cend(), 0.0, [mean = stats.mean](const double sum, const double v) {
            return sum + ((v - mean) * (v - mean));
        });
    stats.stddev = std::sqrt(squared_deviations / static_cast<double>(count));
    return stats;
}

// ---------------------------------------------------------------------------------------------
// Benchmarks
// ---------------------------------------------------------------------------------------------

void Fail(benchmark::State& state, const char* const message)
{
    g_benchmark_failed.store(true, std::memory_order_relaxed);
    state.SkipWithError(message);
}

// fill_payload must be true for EchoMode::kFullPayload: the integrity check relies on the fill.
void RunPingPong(benchmark::State& state, const EchoMode mode, const bool fill_payload)
{
    const auto payload_bytes = static_cast<std::size_t>(state.range(0));

    if (g_master == nullptr)
    {
        Fail(state, "master is not initialised");
        return;
    }
    if (payload_bytes > kMaxPayloadBytes)
    {
        Fail(state, "payload size exceeds the compiled slot capacity");
        return;
    }

    // Reserved up front so that recording a sample never allocates inside the measured loop.
    std::vector<double> one_way_ns;
    one_way_ns.reserve(static_cast<std::size_t>(state.max_iterations));

    // Learning note: `for (auto _ : state)` is Google Benchmark's measured loop. The library
    // decides how many iterations to run: it keeps going until the results are statistically
    // stable or --benchmark_min_time has passed. With UseManualTime() (set in the registration
    // below) it does not time the loop body itself; it adds up the values given to
    // SetIterationTime(). That way only the round trip counts, not the bookkeeping around it.
    const auto wall_start = Clock::now();
    for (auto _ : state)
    {
        if (StopRequested())
        {
            Fail(state, "interrupted");
            break;
        }

        const auto result = g_master->RoundTrip(payload_bytes, mode, fill_payload, kRoundTripTimeout);
        if (result.error != nullptr)
        {
            Fail(state, result.error);
            break;
        }

        // Manual timing: the reported time is the round-trip time itself, not the loop body.
        state.SetIterationTime(std::chrono::duration<double>(result.round_trip).count());
        one_way_ns.push_back(static_cast<double>(result.round_trip.count()) / 2.0);
    }
    const std::chrono::duration<double> wall_elapsed = Clock::now() - wall_start;

    if (one_way_ns.empty())
    {
        return;
    }

    const auto round_trips = static_cast<double>(one_way_ns.size());
    const auto stats = Summarise(one_way_ns);

    // Google Benchmark derives items_per_second / bytes_per_second from the summed round-trip time.
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(payload_bytes));
    state.SetComplexityN(static_cast<std::int64_t>(payload_bytes));

    state.counters["payload_bytes"] = static_cast<double>(payload_bytes);
    state.counters["oneway_min_ns"] = stats.min;
    state.counters["oneway_mean_ns"] = stats.mean;
    state.counters["oneway_p50_ns"] = stats.p50;
    state.counters["oneway_p99_ns"] = stats.p99;
    state.counters["oneway_p999_ns"] = stats.p999;
    state.counters["oneway_max_ns"] = stats.max;
    state.counters["jitter_stddev_ns"] = stats.stddev;
    state.counters["jitter_peak_to_peak_ns"] = stats.max - stats.min;
    // Closed-loop rate including everything outside the timed region (payload fill, wake-up).
    state.counters["wall_round_trips_per_sec"] = round_trips / wall_elapsed.count();
}

// Zero-copy transfer right after the payload was produced. The fill itself is not timed.
static void BM_PingPong(benchmark::State& state)
{
    RunPingPong(state, EchoMode::kHeaderOnly, true);
}

// Zero-copy transfer with untouched payload bytes: no cache eviction by the fill. Expected: O(1).
static void BM_PingPong_NoFill(benchmark::State& state)
{
    RunPingPong(state, EchoMode::kHeaderOnly, false);
}

// Literal "same data back" flow, including the fill and the Reflector's copy. Expected: O(N).
static void BM_PingPong_FullEcho(benchmark::State& state)
{
    RunPingPong(state, EchoMode::kFullPayload, true);
}

// Range(64, 10 << 20) with multiplier 8 yields 64 B, 512 B, 4 KiB, 32 KiB, 256 KiB, 2 MiB, 10 MiB.
// Complexity() makes Google Benchmark fit and report the big-O curve over those sizes.
//
// Learning note: the BENCHMARK macro registers the function in a global list at program start
// (static initialisation), and RunSpecifiedBenchmarks() in main() runs that list. The builder
// calls configure each benchmark:
//   RangeMultiplier/Range  the payload sizes, passed to the function as state.range(0)
//   ArgName                the label in the output, e.g. "BM_PingPong/bytes:4096"
//   UseManualTime          time = sum of SetIterationTime(), see RunPingPong()
//   MinWarmUpTime(0.1)     0.1 s of unrecorded iterations first, so that caches, page tables
//                          and CPU frequency settle before measuring
//   Unit                   print microseconds
//   Complexity             after the last size, fit O(1), O(N), O(N log N), ... and report the
//                          best fit as the "_BigO" row, with its error as the "_RMS" row
BENCHMARK(BM_PingPong)
    ->RangeMultiplier(8)
    ->Range(static_cast<std::int64_t>(kMinPayloadBytes), static_cast<std::int64_t>(kMaxPayloadBytes))
    ->ArgName("bytes")
    ->UseManualTime()
    ->MinWarmUpTime(0.1)
    ->Unit(benchmark::kMicrosecond)
    ->Complexity();

BENCHMARK(BM_PingPong_NoFill)
    ->RangeMultiplier(8)
    ->Range(static_cast<std::int64_t>(kMinPayloadBytes), static_cast<std::int64_t>(kMaxPayloadBytes))
    ->ArgName("bytes")
    ->UseManualTime()
    ->MinWarmUpTime(0.1)
    ->Unit(benchmark::kMicrosecond)
    ->Complexity();

BENCHMARK(BM_PingPong_FullEcho)
    ->RangeMultiplier(8)
    ->Range(static_cast<std::int64_t>(kMinPayloadBytes), static_cast<std::int64_t>(kMaxPayloadBytes))
    ->ArgName("bytes")
    ->UseManualTime()
    ->MinWarmUpTime(0.1)
    ->Unit(benchmark::kMicrosecond)
    ->Complexity();

// ---------------------------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------------------------

enum class Role
{
    kBoth,
    kMaster,
    kReflector,
};

struct Options
{
    Role role{Role::kBoth};
    std::string manifest{"src/benchmark/etc/mw_com_config.json"};
    std::vector<char*> benchmark_args;  // everything that is left for Google Benchmark
    bool valid{true};
};

void PrintUsage(const char* const program)
{
    std::cerr << "Usage: " << program << " [--role=both|master|reflector]"
              << " [--service_instance_manifest=<mw_com_config.json>] [Google Benchmark flags]\n";
}

Options ParseOptions(const int argc, char** const argv)
{
    constexpr std::string_view kRoleFlag{"--role="};
    constexpr std::string_view kManifestFlag{"--service_instance_manifest"};

    Options options{};
    options.benchmark_args.push_back(argv[0]);

    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument{argv[index]};
        if (argument.substr(0U, kRoleFlag.size()) == kRoleFlag)
        {
            const auto role = argument.substr(kRoleFlag.size());
            if (role == "both")
            {
                options.role = Role::kBoth;
            }
            else if (role == "master")
            {
                options.role = Role::kMaster;
            }
            else if (role == "reflector")
            {
                options.role = Role::kReflector;
            }
            else
            {
                std::cerr << "Unknown role '" << role << "'\n";
                options.valid = false;
            }
        }
        else if (argument == kManifestFlag)
        {
            if (index + 1 < argc)
            {
                options.manifest = argv[++index];
            }
            else
            {
                std::cerr << kManifestFlag << " requires a path\n";
                options.valid = false;
            }
        }
        else if (argument.substr(0U, kManifestFlag.size() + 1U) == std::string{kManifestFlag} + "=")
        {
            options.manifest = std::string{argument.substr(kManifestFlag.size() + 1U)};
        }
        else
        {
            options.benchmark_args.push_back(argv[index]);
        }
    }
    return options;
}

// Keeps page faults out of the measurement.
//
// Learning note: Linux maps memory lazily. A freshly allocated page is only backed by RAM when it
// is first touched, and that first touch costs a page fault of a microsecond or more. The kernel
// may also swap pages out later. mlockall(MCL_CURRENT | MCL_FUTURE) pins all current and future
// pages in RAM, a standard step in real-time applications.
//
// Only attempted when the lock cannot run into RLIMIT_MEMLOCK (root, or an unlimited limit). With a
// finite limit mlockall(MCL_FUTURE) may succeed while the process is still small and then make the
// later shared-memory mappings and thread stacks fail, which is far worse than a few page faults.
void LockMemory()
{
    struct rlimit limit
    {
    };
    const bool unlimited = (::getrlimit(RLIMIT_MEMLOCK, &limit) == 0) && (limit.rlim_cur == RLIM_INFINITY);
    if (!unlimited && ::geteuid() != 0)
    {
        std::cerr << "note: memory is not locked (not root and RLIMIT_MEMLOCK is finite). "
                  << "Page faults may show up as latency outliers.\n";
        return;
    }
    if (::mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
    {
        std::cerr << "warning: mlockall() failed (" << std::strerror(errno)
                  << "). Page faults may show up as latency outliers.\n";
    }
}

}  // namespace

int main(int argc, char** argv)
{
    auto options = ParseOptions(argc, argv);
    if (!options.valid)
    {
        PrintUsage(argv[0]);
        return EXIT_FAILURE;
    }

    // Learning note: SIGINT is Ctrl+C, SIGTERM is what `kill` and the run script send. Handling
    // them lets the loops stop cleanly, so the destructors run and the shared-memory files in
    // /dev/shm are removed instead of being left behind.
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    LockMemory();

    // Learning note: the runtime must be initialised before any skeleton or proxy is created. It
    // reads the manifest and starts mw::com's internal threads, including the worker threads
    // that later call the receive handlers.
    //
    // Same call scrample's main.cpp makes (score_communication v0.1.1). Newer releases also accept
    // a score::mw::com::runtime::RuntimeConfiguration - check runtime.h if you bump the version.
    score::StringLiteral runtime_args[] = {"-service_instance_manifest", options.manifest.c_str()};
    score::mw::com::runtime::InitializeRuntime(2, runtime_args);

    const bool run_master = options.role != Role::kReflector;
    const bool run_reflector = options.role != Role::kMaster;

    auto master = run_master ? std::make_unique<Master>() : nullptr;
    auto reflector = run_reflector ? std::make_unique<Reflector>() : nullptr;

    // Both sides offer first and connect afterwards, so the start order of two processes (or of the
    // two halves of one process) does not matter.
    if ((master != nullptr && !master->Offer()) || (reflector != nullptr && !reflector->Offer()))
    {
        return EXIT_FAILURE;
    }
    if ((reflector != nullptr && !reflector->Connect()) || (master != nullptr && !master->Connect()))
    {
        return EXIT_FAILURE;
    }

    if (master == nullptr)
    {
        std::cout << "Reflector ready. Waiting for SIGINT/SIGTERM..." << std::endl;
        while (!StopRequested())
        {
            std::this_thread::sleep_for(100ms);
        }
        std::cout << "Reflector stopped, failed echoes: " << reflector->FailedEchoes() << std::endl;
        return EXIT_SUCCESS;
    }

    if (!master->Handshake())
    {
        return EXIT_FAILURE;
    }

    auto benchmark_argc = static_cast<int>(options.benchmark_args.size());
    options.benchmark_args.push_back(nullptr);
    benchmark::Initialize(&benchmark_argc, options.benchmark_args.data());
    if (benchmark::ReportUnrecognizedArguments(benchmark_argc, options.benchmark_args.data()))
    {
        PrintUsage(argv[0]);
        return EXIT_FAILURE;
    }

    benchmark::AddCustomContext("transport", "score::mw::com (LoLa shared memory)");
    benchmark::AddCustomContext("topology", options.role == Role::kBoth ? "single process" : "two processes");
    benchmark::AddCustomContext("slot_capacity_bytes", std::to_string(kMaxPayloadBytes));

    // Learning note: BENCHMARK functions are plain functions without parameters for our objects,
    // so they reach the Master through this global. It is a raw, non-owning pointer:
    // `master` (the unique_ptr) still owns the object and destroys it at the end of main().
    g_master = master.get();
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    g_master = nullptr;

    if (reflector != nullptr && reflector->FailedEchoes() != 0U)
    {
        std::cerr << "Reflector failed to echo " << reflector->FailedEchoes() << " ping(s)\n";
        return EXIT_FAILURE;
    }
    return g_benchmark_failed.load(std::memory_order_relaxed) ? EXIT_FAILURE : EXIT_SUCCESS;
}
