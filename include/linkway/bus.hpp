// linkway/bus.hpp — the user-facing API: typed publish/subscribe and request/reply.
//
// v2 additions (all opt-in; v1 behaviour is the default):
//   Receive::Poll       no receive thread: the application calls poll() on its own
//                       thread (run-to-completion, like Aeron or DPDK). Shared memory only.
//   Dispatch::Inline    a handler runs on the receiving thread (no hand-off to a
//                       worker, no copy): the low-latency path. It must be fast.
//   spin                spin this long before sleeping (receive thread, workers, call()).
//   call<Req,Rep>()     synchronous request that spins/polls instead of sleeping on a future.
//   reserve<T>()        zero-copy publish: build the message directly in shared memory.
//   checksum            CRC32C on every frame (hardware-accelerated).
#pragma once

#include "linkway/codec.hpp"
#include "linkway/detail/cpu_relax.hpp"
#include "linkway/detail/payload.hpp"
#include "linkway/error.hpp"
#include "linkway/event_loop.hpp"
#include "linkway/executor.hpp"
#include "linkway/message.hpp"
#include "linkway/metrics.hpp"
#include "linkway/transport.hpp"
#include "linkway/vocab.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <ostream>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace linkway {

using SubscriptionId = std::uint64_t;

enum class Receive { Thread, Poll };
enum class Dispatch { Worker, Inline };

struct BusConfig {
    std::size_t worker_threads = 4;
    std::size_t queue_capacity = 1024;
    std::size_t max_message_bytes = 16u << 20;   // a corrupt length can never force more
    Overflow overflow = Overflow::Block;
    bool drain_on_shutdown = true;
    std::string thread_prefix = "worker";        // worker threads: worker-0 .. worker-N
    std::string loop_thread_name = "lw-loop";    // two Buses in one process: give each its own name
    // ---- v2 ----
    Receive receive = Receive::Thread;
    std::chrono::microseconds spin{0};           // spin-then-park budget (0 = sleep at once)
    bool checksum = false;                       // CRC32C per frame; both ends must match
    bool batch_dispatch = true;                  // hand each received burst to the executor at once
    // false: create() delivers nothing until start_receiving(). Subscribe first,
    // then start, and no early message is dropped for want of a subscriber.
    bool start_receiving = true;
};

class Bus;

// A message being built in place (zero-copy). Obtained from Bus::reserve<T>().
// Fill it through ->, then commit(). Destroyed without commit(): cancelled.
// A Loan must not outlive its Bus, and a thread should hold one Loan at a time.
template <class T>
class Loan {
public:
    T* operator->() noexcept { return obj_; }
    T& operator*() noexcept { return *obj_; }
    bool zero_copy() const noexcept { return zero_copy_; }
    // (single-producer rings: the Loan holds the Bus's send lock until commit)
    std::error_code commit();
    ~Loan();
    Loan(Loan&& o) noexcept;
    Loan& operator=(Loan&&) = delete;
    Loan(const Loan&) = delete;

private:
    friend class Bus;
    explicit Loan(Bus* b) noexcept : bus_(b) {}
    Bus* bus_;
    WriteClaim claim_{};
    T* obj_ = nullptr;
    bool zero_copy_ = false;
    bool done_ = false;
    std::unique_lock<std::mutex> lock_;           // held from reserve() to commit() on a single-producer ring
    alignas(T) unsigned char local_[sizeof(T)];   // fallback storage when the transport can't lend memory
};

class Bus {
public:
    using Config = BusConfig;

    // Bus owns threads that capture `this`, so it can never move: it lives
    // behind a unique_ptr. create() returns once the transport is started.
    static Result<std::unique_ptr<Bus>> create(Transport transport, Config cfg = {});
    ~Bus();
    Bus(const Bus&) = delete;
    Bus& operator=(const Bus&) = delete;

    // ---- publish / subscribe ---------------------------------------------
    template <class T>
    std::error_code publish(const T& msg);

    // Zero-copy publish over shared memory (a copying fallback elsewhere, so the
    // same code works on every transport). WouldBlock if the ring is full.
    template <class T>
    Result<Loan<T>> reserve();

    // handler: void(const T&). Worker: runs on a worker thread (default).
    // Inline: runs on the receiving thread, reading the message in place.
    // After unsubscribe() returns no NEW deliveries start; one already queued may still run.
    template <class T, class Handler>
    SubscriptionId subscribe(Handler&& handler, Dispatch d = Dispatch::Worker);
    void unsubscribe(SubscriptionId id);

    // ---- request / reply ---------------------------------------------------
    template <class Req, class Rep>
    std::future<Result<Rep>> request(const Req& req, std::chrono::milliseconds timeout);

    // Synchronous request. In poll mode it drives poll() itself while waiting;
    // otherwise it spins for Config::spin, then sleeps. No future, no allocation
    // for the result.
    template <class Req, class Rep>
    Result<Rep> call(const Req& req, std::chrono::milliseconds timeout);

    // handler: Rep(const Req&). Its return value is the reply.
    template <class Req, class Rep, class Handler>
    void serve(Handler&& handler, Dispatch d = Dispatch::Worker);

    // ---- v2: receive-side control -------------------------------------------
    // Poll mode only: receive and dispatch up to `max_frames` on THIS thread.
    // Returns how many frames were handled. If another thread is polling, or
    // this is a nested call from a handler, returns 0 at once: the ring has ONE
    // consumer, and a re-entrant poll would deliver the same slot twice (found in
    // review: an Inline handler calling call() corrupted the ring).
    std::size_t poll(std::size_t max_frames = 64);
    // The largest payload that fits this Bus's transport (and max_message_bytes).
    std::size_t max_payload() const noexcept;
    // With BusConfig::start_receiving = false: begin delivering (any thread, once).
    void start_receiving();

    // Untyped messages, for protocols layered on the Bus (e.g. the durable log).
    std::error_code publish_raw(TypeId id, ByteView payload);
    SubscriptionId subscribe_raw(TypeId id, std::function<void(ByteView)> handler, Dispatch d = Dispatch::Worker);
    // Called on every link state change (loop/rx thread). Keep it short.
    void on_state_change(std::function<void(LinkState)> fn);

    // ---- observability -----------------------------------------------------
    const Metrics& metrics() const noexcept { return metrics_; }
    LinkState state() const;
    bool wait_until_connected(std::chrono::milliseconds timeout) const;
    void dump_state(std::ostream& os) const;
    // `kill -USR1 <pid>` writes dump_state() to out_fd. One LIVE Bus per process
    // owns it at a time; when that Bus is destroyed, another may take over.
    bool enable_sigusr1_dump(int out_fd = 2);
    const Config& config() const noexcept { return cfg_; }

private:
    template <class T> friend class Loan;
    using Handler = std::function<void(ByteView)>;
    using ServerFn = std::function<void(ByteView, std::uint64_t correlation)>;
    struct Subscription { SubscriptionId id; std::shared_ptr<const Handler> fn; Dispatch dispatch; };
