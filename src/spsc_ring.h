#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <vector>

// lock-free single-producer / single-consumer ring buffer.
// one thread calls push(), exactly one other thread calls pop().
// capacity must be a power of two so we can mask instead of mod.
template <typename T>
class SpscRing {
public:
    explicit SpscRing(size_t capacityPow2)
        : buf_(capacityPow2), mask_(capacityPow2 - 1) {}

    // returns how many items were actually written (may be less if full)
    size_t push(const T* data, size_t n) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_acquire);
        const size_t free = buf_.size() - (head - tail);
        n = std::min(n, free);
        for (size_t i = 0; i < n; ++i) buf_[(head + i) & mask_] = data[i];
        head_.store(head + n, std::memory_order_release);
        return n;
    }

    // returns how many items were actually read (may be less if empty)
    size_t pop(T* out, size_t n) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t head = head_.load(std::memory_order_acquire);
        n = std::min(n, head - tail);
        for (size_t i = 0; i < n; ++i) out[i] = buf_[(tail + i) & mask_];
        tail_.store(tail + n, std::memory_order_release);
        return n;
    }

    // approximate when called from a third thread, fine for ui/debug
    size_t size() const {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }
    size_t capacity() const { return buf_.size(); }

private:
    std::vector<T> buf_;
    size_t mask_;
    // separate cache lines so producer and consumer don't fight over one
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};
