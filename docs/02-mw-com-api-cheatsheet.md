# 02 score::mw::com API cheat sheet

The calls the benchmark uses, next to the `ara::com` concept they correspond to. Signatures were
checked against the headers of `score_communication` **0.1.1**. Newer releases differ in places
(runtime initialisation in particular), so re-check after a version bump.

## Build

| What | Value |
|---|---|
| Bazel dependency | `@score_communication//score/mw/com` |
| Headers | `score/mw/com/types.h`, `score/mw/com/runtime.h` |
| Required flags | `--config=host` in this repository (see `.bazelrc`) |
| Manifest | JSON file, passed as `-service_instance_manifest <path>` |

## Defining a service

There is no IDL or code generator. A service interface is a class template; the middleware
instantiates it once as a proxy and once as a skeleton.

```cpp
template <typename Trait>
class TopicAInterface : public Trait::Base
{
  public:
    using Trait::Base::Base;
    typename Trait::template Event<PingPongPayload> ping_{*this, "ping"};
};

using TopicASkeleton = score::mw::com::AsSkeleton<TopicAInterface>;
using TopicAProxy    = score::mw::com::AsProxy<TopicAInterface>;
```

The event name (`"ping"`) must match `eventName` in the manifest.

## Calls

All `Result` types have `has_value()`, `value()` and `error()`. `ResultBlank` carries no value.

| Step | `ara::com` concept | `score::mw::com` call | Returns |
|---|---|---|---|
| Start the runtime | `ara::core::Initialize` | `runtime::InitializeRuntime(argc, argv)` | `void` |
| Name an instance | `InstanceSpecifier` | `InstanceSpecifier::Create(name)` | `Result<InstanceSpecifier>` |
| Create a provider | skeleton constructor | `Skeleton::Create(specifier)` | `Result<Skeleton>` |
| Offer it | `OfferService()` | `skeleton.OfferService()` | `ResultBlank` |
| Get a slot | `event.Allocate()` | `skeleton.event_.Allocate()` | `Result<SampleAllocateePtr<T>>` |
| Publish | `event.Send()` | `skeleton.event_.Send(std::move(sample))` | `ResultBlank` |
| Discover | `FindService()` | `Proxy::FindService(specifier)` | `Result<ServiceHandleContainer<Handle>>` |
| Create a consumer | proxy constructor | `Proxy::Create(handle)` | `Result<Proxy>` |
| Subscribe | `event.Subscribe()` | `proxy.event_.Subscribe(max_sample_count)` | `ResultBlank` |
| Get notified | `SetReceiveHandler()` | `proxy.event_.SetReceiveHandler(callable)` | `ResultBlank` |
| Read | `GetNewSamples()` | `proxy.event_.GetNewSamples(callable, max_count)` | `Result<std::size_t>` |
| Stop | `Unsubscribe()` | `proxy.event_.Unsubscribe()` | `void` |
| Withdraw | `StopOfferService()` | `skeleton.StopOfferService()` | `void` |

The callable passed to `GetNewSamples` receives a `score::mw::com::SamplePtr<T>` by value.

## Things that are easy to get wrong

- **`Send(const T&)` also exists** and copies the object into a slot. Zero-copy means
  `Allocate()` followed by `Send(std::move(sample))`.
- **A received sample cannot be re-published.** A `SamplePtr` belongs to the event it came from.
  Forwarding data on another event means allocating there and copying.
- **Proxy and skeleton are move-only and have no default constructor.** Hold them in a
  `std::unique_ptr` or `std::optional`, and put them in their final place before registering a
  handler that captures `this`.
- **Register the handler before subscribing**, otherwise a sample that arrives in between is not
  announced.
- **`numberOfSampleSlots` must exceed what subscribers may hold.** With every slot referenced by a
  subscriber, `Allocate()` fails. The benchmark uses 5 slots and `Subscribe(2)`.
- **A receive handler can overlap with itself.** Notifications are dispatched on a pool of two
  worker threads (`ThreadPool(2U, ...)` in `impl/bindings/lola/messaging/notify_event_handler.cpp`),
  so the invocation for the next sample can start while the previous one is still returning. Proxy
  events and skeleton events are not safe for concurrent use. A handler that calls
  `GetNewSamples()`, `Allocate()` or `Send()` must take a lock first. The symptom of getting this
  wrong is rare, silent loss of samples. See [03, section 11](03-benchmark-design.md).
- **Runtime initialisation at 0.1.1** takes `(std::int32_t argc, score::StringLiteral argv[])` and
  looks for `-service_instance_manifest`. scrample passes exactly two elements: the flag and the
  path. An overload taking a `RuntimeConfiguration` also exists.

## Manifest

Two parts, both required: `serviceTypes` describes the interface (service id, event ids),
`serviceInstances` binds an instance specifier to a concrete instance (instance id, binding `SHM`,
slots per event). `src/benchmark/etc/mw_com_config.json` is a minimal example with two services.
VS Code validates it against the upstream schema (configured in `.vscode/settings.json`).
