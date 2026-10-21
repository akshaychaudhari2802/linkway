// linkway/detail/bounded_queue.hpp
// Mutex + condition-variable bounded FIFO. Producers never block (the I/O thread
// must not); consumers (worker threads) block in pop().
// v2: push_batch() (one lock and a bounded number of wake-ups for a whole burst)
// and try_pop() (so a worker can spin briefly before sleeping).
#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <utility>

namespace linkway::detail {

template <class T>
class BoundedQueue {
public:
    enum class Push { Ok, Full, Closed };

    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {}

    // Non-blocking. Full when size() == capacity.
    Push try_push(T&& v) {
        {
            std::lock_guard lock(mu_);
            if (closed_) return Push::Closed;
            if (q_.size() >= capacity_) { was_full_ = true; return Push::Full; }
            q_.push_back(std::move(v));
        }
        not_empty_.notify_one();
        return Push::Ok;
    }

    // Non-blocking. Moves items[0..k) in, in order, where k is how many fit
    // (0 if closed; `closed` tells which). Items not taken are left intact.
    std::size_t push_batch(T* items, std::size_t n, bool* closed = nullptr) {
        std::size_t k = 0;
        {
            std::lock_guard lock(mu_);
            if (closed_) { if (closed) *closed = true; return 0; }
            for (; k < n && q_.size() < capacity_; ++k) q_.push_back(std::move(items[k]));
            if (k < n) was_full_ = true;
        }
        // Wake at most as many sleepers as there is new work (notify_one is
        // cheap when nobody waits). Capped: a handful of workers is typical.
        for (std::size_t i = 0; i < k && i < 8; ++i) not_empty_.notify_one();
        return k;
    }

    enum class Evict { Pushed, PushedEvictingOldest, Closed };

    // Non-blocking. When full, evicts the oldest element (into *evicted if given).
    Evict push_evicting_oldest(T&& v, T* evicted) {
        Evict r = Evict::Pushed;
        {
            std::lock_guard lock(mu_);
            if (closed_) return Evict::Closed;
            if (q_.size() >= capacity_) {
                if (evicted) *evicted = std::move(q_.front());
                q_.pop_front();
