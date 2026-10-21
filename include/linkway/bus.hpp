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
    struct Server { std::shared_ptr<const ServerFn> fn; Dispatch dispatch; };
    // Copy-on-write routing table: the receive path reads a snapshot without
    // locking or copying (v1 copied the subscriber vector for every message).
    struct Table {
        std::unordered_map<TypeId, std::vector<Subscription>> subs;
        std::unordered_map<TypeId, Server> servers;
    };
    struct Pending {
        std::function<void(ByteView)> on_reply;
        std::function<void(std::error_code)> on_error;
        TypeId reply_type = 0;   // a reply of any other type is rejected (not just checked by size)
    };

    Bus(Transport transport, Config cfg);

    std::error_code send(TypeId wire_type, std::uint64_t correlation, ByteView payload);
    void on_frame(const FrameHeader& h, ByteView payload);   // receiving thread
    void on_batch_end();
    void on_state(LinkState s, std::error_code why);
    void enqueue(Executor::Task&& task);
    void flush_batch();
    void drain_backlog();
    void run_inline(const Handler& fn, ByteView payload) noexcept;
    std::uint64_t add_pending(Pending p);
    bool complete_request(std::uint64_t corr, TypeId type, ByteView payload);
    void send_reply(TypeId wire_type, std::uint64_t corr, ByteView payload, bool inline_dispatch);
    bool fail_request(std::uint64_t corr, std::error_code ec);
    void fail_all_pending(std::error_code ec);
    void update_table(const std::function<void(Table&)>& edit);
    std::shared_ptr<const Table> table() const { return std::atomic_load(&table_); }
    const Table& rx_table();   // receiving thread: a cached snapshot, refreshed only when the table changes
    void release_sigusr1_dump() noexcept;

    template <class T>
    static ByteView bytes_of(const T& v) noexcept {
        return {reinterpret_cast<const std::byte*>(&v), sizeof(T)};   // std::byte may alias anything
    }
    // Read a T from received bytes: in place when aligned (no copy), else via a copy.
    template <class T, class H>
    static void with_message(ByteView payload, H& h);

    // Declaration order = construction order, and REVERSE destruction order.
    // Threads (executor_, loop_thread_) start LAST, once every member they
    // touch exists. payload_pool_ is declared before backlog_ and executor_,
    // whose queued tasks hold payloads: they must be destroyed first.
    const Config cfg_;
    Metrics metrics_;
    Transport transport_;
    EventLoop loop_;
    detail::PayloadPool payload_pool_;

    std::mutex table_mu_;                        // serialises writers of table_
    std::shared_ptr<const Table> table_;         // readers: std::atomic_load
    std::atomic<std::uint64_t> table_version_{1};
    std::shared_ptr<const Table> rx_table_;      // receiving thread only
    std::uint64_t rx_version_ = 0;               // receiving thread only
    std::atomic<SubscriptionId> next_sub_{1};

    mutable std::mutex pending_mu_;
    std::unordered_map<std::uint64_t, Pending> pending_;
    std::atomic<std::uint64_t> next_corr_{1};

    std::vector<Executor::Task> batch_;          // receiving thread only
    mutable std::mutex backlog_mu_;
    std::deque<Executor::Task> backlog_;         // tasks the executor had no room for (Block policy)
    bool paused_ = false;                        // guarded by backlog_mu_

    mutable std::mutex state_mu_;
    mutable std::condition_variable state_cv_;
    LinkState state_ = LinkState::Connecting;
    bool ever_connected_ = false;
    std::vector<std::function<void(LinkState)>> state_listeners_;   // guarded by state_mu_

    std::atomic<bool> stopping_{false};
    std::atomic<std::thread::id> poller_{};       // the thread inside poll(), if any
    const bool serialize_sends_;                  // single-producer transport: one sender at a time
    mutable std::mutex send_mu_;
    bool owns_dump_ = false;                     // this Bus's loop reads the SIGUSR1 eventfd
    Executor executor_;
    std::thread loop_thread_;
};

// ======================================================= template definitions

template <class T, class H>
void Bus::with_message(ByteView payload, H& h) {
    if (payload.size() != sizeof(T)) return;   // defensive: peer built with a different T
    if (reinterpret_cast<std::uintptr_t>(payload.data()) % alignof(T) == 0) {
        // In place, no copy. T is trivially copyable (LINKWAY_MESSAGE checks), an
        // implicit-lifetime type: C++20 made reading it from raw bytes well defined
        // (P0593, implicit object creation), and GCC/Clang compile C++17 the same way.
        h(*std::launder(reinterpret_cast<const T*>(payload.data())));
    } else {
        T msg;
        std::memcpy(&msg, payload.data(), sizeof(T));
        h(static_cast<const T&>(msg));
    }
}

template <class T>
std::error_code Bus::publish(const T& msg) {
    static_assert(is_registered_v<T>,
                  "publish<T>: T is not registered. Add LINKWAY_MESSAGE(T, id) to a shared header.");
    if (stopping_.load(std::memory_order_acquire)) return ErrorCode::AlreadyStopped;
    return send(message_traits<T>::id, 0, bytes_of(msg));
}

template <class T>
Result<Loan<T>> Bus::reserve() {
    static_assert(is_registered_v<T>, "reserve<T>: T is not registered with LINKWAY_MESSAGE");
    static_assert(alignof(T) <= 8, "reserve<T>: in-place messages must have alignment <= 8");
    if (stopping_.load(std::memory_order_acquire)) return Err(ErrorCode::AlreadyStopped);
    Loan<T> loan(this);
    if (transport_.supports_zero_copy()) {
        const std::size_t bytes = frame_size(message_traits<T>::id, sizeof(T), cfg_.checksum);
        if (serialize_sends_) loan.lock_ = std::unique_lock<std::mutex>(send_mu_);
        WriteClaim c = transport_.claim(bytes);
        if (!c.data) return Err(c.capacity && bytes > c.capacity ? ErrorCode::MessageTooLarge : ErrorCode::WouldBlock);
        encode_header(c.data, message_traits<T>::id, 0, sizeof(T), cfg_.checksum);
        loan.obj_ = ::new (static_cast<void*>(c.data + kBaseHeaderSize)) T{};   // payload is 8-byte aligned in a slot
        loan.claim_ = c;
        loan.zero_copy_ = true;
    } else {
        loan.obj_ = ::new (static_cast<void*>(loan.local_)) T{};
    }
    return loan;
}

template <class T>
std::error_code Loan<T>::commit() {
    if (done_) return ErrorCode::InvalidFrame;
    done_ = true;
    if (!zero_copy_) return bus_->publish(*obj_);
    const std::size_t body = kBaseHeaderSize + sizeof(T);
    if (bus_->cfg_.checksum) seal_frame(claim_.data, body);
    const std::size_t bytes = body + (bus_->cfg_.checksum ? kChecksumSize : 0);
    bus_->transport_.commit(claim_, bytes);
    bus_->metrics_.messages_sent.fetch_add(1, std::memory_order_relaxed);
    bus_->metrics_.bytes_sent.fetch_add(bytes, std::memory_order_relaxed);
    bus_->metrics_.zero_copy_sent.fetch_add(1, std::memory_order_relaxed);
    if (lock_.owns_lock()) lock_.unlock();
    return {};
}

template <class T>
Loan<T>::~Loan() {
    if (zero_copy_ && !done_ && bus_) bus_->transport_.commit(claim_, 0);   // cancel: release the slot empty
}

template <class T>
Loan<T>::Loan(Loan&& o) noexcept
    : bus_(std::exchange(o.bus_, nullptr)), claim_(o.claim_), zero_copy_(o.zero_copy_), done_(o.done_),
      lock_(std::move(o.lock_)) {
    o.done_ = true;
    if (zero_copy_) {
        obj_ = o.obj_;
    } else if (o.obj_) {
        std::memcpy(local_, o.local_, sizeof(T));
        obj_ = std::launder(reinterpret_cast<T*>(local_));
    }
}

template <class T, class H>
SubscriptionId Bus::subscribe(H&& handler, Dispatch d) {
    static_assert(is_registered_v<T>, "subscribe<T>: T is not registered with LINKWAY_MESSAGE");
    static_assert(std::is_invocable_v<H&, const T&>,
                  "subscribe<T>: the handler must be callable as void(const T&)");
    // Erase the type here, where T is still known. From the outside every handler
    // is void(ByteView), so one table holds handlers for every message type.
    auto erased = std::make_shared<const Handler>(
        [h = std::forward<H>(handler)](ByteView payload) mutable { with_message<T>(payload, h); });
    const SubscriptionId id = next_sub_.fetch_add(1, std::memory_order_relaxed);
    update_table([&](Table& t) { t.subs[message_traits<T>::id].push_back(Subscription{id, erased, d}); });
    return id;
}

