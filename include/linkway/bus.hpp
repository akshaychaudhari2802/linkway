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
