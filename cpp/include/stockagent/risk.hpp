#pragma once

#include "stockagent/types.hpp"

#include <atomic>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace stockagent {

struct RiskConfig {
    Quantity max_order_quantity{100};
    Quantity max_absolute_position{500};
    std::uint64_t max_gross_notional_ticks{100'000'000};
    std::int64_t max_loss_ticks{500'000};
    TimestampNs max_event_gap_ns{1'000'000'000};
};

class RiskManager {
  public:
    explicit RiskManager(RiskConfig config) : config_(config) {
        constexpr auto safe_notional_limit =
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max() / 4);
        if (config_.max_order_quantity <= 0 ||
            config_.max_absolute_position <= 0 ||
            config_.max_gross_notional_ticks == 0 ||
            config_.max_gross_notional_ticks > safe_notional_limit ||
            config_.max_loss_ticks < 0) {
            throw std::invalid_argument("invalid risk configuration");
        }
    }

    void set_kill_switch(const bool enabled) noexcept {
        kill_switch_.store(enabled ? 1U : 0U, std::memory_order_relaxed);
    }

    [[nodiscard]] bool kill_switch() const noexcept {
        return kill_switch_.load(std::memory_order_relaxed) != 0U;
    }

    [[nodiscard]] RiskRejectReason validate_market(
        const MarketTick& tick) noexcept {
        if (tick.bid_ticks <= 0 || tick.ask_ticks <= 0 ||
            tick.bid_ticks > tick.ask_ticks || tick.bid_quantity <= 0 ||
            tick.ask_quantity <= 0) {
            return RiskRejectReason::BadQuote;
        }
        if (has_market_ &&
            (tick.sequence <= last_sequence_ ||
             tick.timestamp_ns <= last_timestamp_ns_)) {
            return RiskRejectReason::NonMonotonicEvent;
        }
        if (has_market_ && config_.max_event_gap_ns > 0 &&
            (tick.timestamp_ns - last_timestamp_ns_) >
                config_.max_event_gap_ns) {
            // Reject the discontinuity itself but adopt it as the new baseline,
            // allowing the next ordered tick to resume safely.
            last_sequence_ = tick.sequence;
            last_timestamp_ns_ = tick.timestamp_ns;
            return RiskRejectReason::StaleEvent;
        }

        has_market_ = true;
        last_sequence_ = tick.sequence;
        last_timestamp_ns_ = tick.timestamp_ns;
        return RiskRejectReason::None;
    }

    [[nodiscard]] RiskRejectReason check_order(
        const StrategyDecision& decision, const MarketTick& tick,
        const LedgerSnapshot& ledger) const noexcept {
        if (decision.quantity <= 0) {
            return RiskRejectReason::InvalidOrder;
        }
        if (config_.max_order_quantity <= 0 ||
            decision.quantity > config_.max_order_quantity) {
            return RiskRejectReason::OrderSize;
        }
        const Quantity displayed_quantity =
            decision.side == Side::Buy ? tick.ask_quantity
                                       : tick.bid_quantity;
        if (decision.quantity > displayed_quantity) {
            return RiskRejectReason::InsufficientLiquidity;
        }

        const auto signed_quantity =
            decision.side == Side::Buy ? decision.quantity
                                       : -decision.quantity;
        if ((signed_quantity > 0 &&
             ledger.position >
                 std::numeric_limits<Quantity>::max() - signed_quantity) ||
            (signed_quantity < 0 &&
             ledger.position <
                 std::numeric_limits<Quantity>::min() - signed_quantity)) {
            return RiskRejectReason::PositionLimit;
        }
        const Quantity new_position = ledger.position + signed_quantity;
        const auto old_absolute = magnitude(ledger.position);
        const auto absolute_position = magnitude(new_position);
        const bool same_direction =
            new_position == 0 ||
            (ledger.position > 0 && new_position > 0) ||
            (ledger.position < 0 && new_position < 0);
        const bool reduces_exposure =
            ledger.position != 0 && same_direction &&
            absolute_position < old_absolute;

        if (kill_switch() && !reduces_exposure) {
            return RiskRejectReason::KillSwitch;
        }
        if (absolute_position >
                static_cast<std::uint64_t>(
                    config_.max_absolute_position) &&
            !reduces_exposure) {
            return RiskRejectReason::PositionLimit;
        }

        const Price execution_price =
            decision.side == Side::Buy ? tick.ask_ticks : tick.bid_ticks;
        const auto unsigned_price =
            static_cast<std::uint64_t>(execution_price);
        const auto unsigned_quantity =
            static_cast<std::uint64_t>(decision.quantity);
        constexpr auto arithmetic_notional_limit =
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max() / 2);
        if (unsigned_price >
            (arithmetic_notional_limit / unsigned_quantity)) {
            return RiskRejectReason::NotionalLimit;
        }
        if (!reduces_exposure &&
            (unsigned_price >
                 (config_.max_gross_notional_ticks / unsigned_quantity) ||
             (absolute_position > 0 &&
              unsigned_price >
                  (config_.max_gross_notional_ticks /
                   absolute_position)))) {
            return RiskRejectReason::NotionalLimit;
        }

        if (config_.max_loss_ticks > 0 &&
            ledger.total_pnl_ticks <= -config_.max_loss_ticks &&
            !reduces_exposure) {
            return RiskRejectReason::LossLimit;
        }
        return RiskRejectReason::None;
    }

  private:
    [[nodiscard]] static constexpr std::uint64_t magnitude(
        const Quantity value) noexcept {
        return value < 0
                   ? static_cast<std::uint64_t>(-(value + 1)) + 1U
                   : static_cast<std::uint64_t>(value);
    }

    RiskConfig config_;
    Sequence last_sequence_{};
    TimestampNs last_timestamp_ns_{};
    bool has_market_{false};
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                  "kill switch requires lock-free uint32 atomics");
    std::atomic<std::uint32_t> kill_switch_{0};
};

}  // namespace stockagent
