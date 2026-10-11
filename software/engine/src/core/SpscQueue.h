#pragma once

#include <array>
#include <atomic>
#include <cstddef>

namespace mbx {

// Lock-free single-producer / single-consumer ring buffer.
// One thread calls push(), one (other) thread calls pop(). Neither call
// allocates, locks or blocks, so pop() is safe in the audio thread.
template <typename T, std::size_t Capacity>
class SpscQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");

public:
    // Producer side. Returns false when the queue is full (event dropped).
    bool push(const T& item) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= Capacity) return false;
        buffer_[head & (Capacity - 1)] = item;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns false when the queue is empty.
    bool pop(T& item) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        if (tail == head) return false;
        item = buffer_[tail & (Capacity - 1)];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

private:
    std::array<T, Capacity> buffer_{};
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};

} // namespace mbx
