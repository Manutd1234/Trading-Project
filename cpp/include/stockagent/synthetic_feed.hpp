#pragma once

#include "stockagent/types.hpp"

#include <algorithm>
#include <cstdint>

namespace stockagent {

class SyntheticFeed {
  public:
    explicit SyntheticFeed(std::uint64_t seed = 42,
                           Price initial_mid_ticks = 100'000) noexcept
        : state_(seed == 0 ? 42 : seed), mid_ticks_(initial_mid_ticks) {}

    [[nodiscard]] MarketTick next() noexcept {
        ++sequence_;
        const auto random = next_random();
        const auto phase = (sequence_ / 250U) % 4U;
        const Price drift =
            phase == 0U ? 3 : (phase == 1U ? -3 : (phase == 2U ? 1 : -1));
        const Price noise = static_cast<Price>(random % 5U) - 2;
        mid_ticks_ = std::max<Price>(1'000, mid_ticks_ + drift + noise);

        const Price half_spread =
            4 + static_cast<Price>((random >> 8U) % 3U);
        Quantity bid_quantity = 500;
        Quantity ask_quantity = 500;
        if (phase == 0U || phase == 2U) {
            bid_quantity =
                800 + static_cast<Quantity>((random >> 16U) % 200U);
            ask_quantity =
                100 + static_cast<Quantity>((random >> 24U) % 100U);
        } else {
            bid_quantity =
                100 + static_cast<Quantity>((random >> 16U) % 100U);
            ask_quantity =
                800 + static_cast<Quantity>((random >> 24U) % 200U);
        }

        return MarketTick{
            sequence_,
            1'000'000'000U + (sequence_ * 1'000U),
            mid_ticks_ - half_spread,
            bid_quantity,
            mid_ticks_ + half_spread,
            ask_quantity,
        };
    }

  private:
    [[nodiscard]] std::uint64_t next_random() noexcept {
        state_ ^= state_ << 13U;
        state_ ^= state_ >> 7U;
        state_ ^= state_ << 17U;
        return state_;
    }

    std::uint64_t state_;
    Sequence sequence_{};
    Price mid_ticks_;
};

}  // namespace stockagent
