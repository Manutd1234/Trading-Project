#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace stockagent {

// Fixed-capacity, single-producer/single-consumer queue. Exactly one thread may
// call try_push and exactly one thread may call try_pop.
template <typename T, std::size_t Capacity>
class SpscQueue {
    static_assert(Capacity >= 2, "SPSC queue capacity must be at least two");
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "SPSC queue capacity must be a power of two");
    static_assert(std::is_default_constructible_v<T>,
                  "SPSC queue elements must be default constructible");

  public:
    SpscQueue() = default;
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    [[nodiscard]] bool try_push(const T& value) noexcept(
        std::is_nothrow_copy_assignable_v<T>) {
        const auto head = head_.value.load(std::memory_order_relaxed);
        const auto tail = tail_.value.load(std::memory_order_acquire);
        if ((head - tail) == Capacity) {
            return false;
        }
        storage_[head & mask] = value;
        head_.value.store(head + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_pop(T& value) noexcept(
        std::is_nothrow_copy_assignable_v<T>) {
        const auto tail = tail_.value.load(std::memory_order_relaxed);
        const auto head = head_.value.load(std::memory_order_acquire);
        if (tail == head) {
            return false;
        }
        value = storage_[tail & mask];
        tail_.value.store(tail + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t approximate_size() const noexcept {
        const auto head = head_.value.load(std::memory_order_acquire);
        const auto tail = tail_.value.load(std::memory_order_acquire);
        return head - tail;
    }

    [[nodiscard]] constexpr std::size_t capacity() const noexcept {
        return Capacity;
    }

  private:
    static constexpr std::size_t cache_line_size = 64;
    static constexpr std::size_t mask = Capacity - 1;

    struct alignas(cache_line_size) Cursor {
        std::atomic<std::size_t> value{0};
    };

    std::array<T, Capacity> storage_{};
    Cursor head_{};
    Cursor tail_{};
};

}  // namespace stockagent
