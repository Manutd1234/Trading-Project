#include "stockagent/engine.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace stockagent {
namespace {

[[nodiscard]] constexpr bool checked_add(
    const std::int64_t left, const std::int64_t right,
    std::int64_t& result) noexcept {
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    if ((right > 0 && left > maximum - right) ||
        (right < 0 && left < minimum - right)) {
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] constexpr bool checked_subtract(
    const std::int64_t left, const std::int64_t right,
    std::int64_t& result) noexcept {
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    if ((right > 0 && left < minimum + right) ||
        (right < 0 && left > maximum + right)) {
        return false;
    }
    result = left - right;
    return true;
}

[[nodiscard]] constexpr bool checked_multiply(
    const std::int64_t left, const std::int64_t right,
    std::int64_t& result) noexcept {
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    if (left == 0 || right == 0) {
        result = 0;
        return true;
    }
    if (left > 0) {
        if ((right > 0 && left > maximum / right) ||
            (right < 0 && right < minimum / left)) {
            return false;
        }
    } else {
        if ((right > 0 && left < minimum / right) ||
            (right < 0 && left < maximum / right)) {
            return false;
        }
    }
    result = left * right;
    return true;
}

}  // namespace

TradingEngine::TradingEngine(EngineConfig config)
    : strategy_(config.strategy), risk_(config.risk) {}

std::optional<Fill> TradingEngine::on_market_tick(
    const MarketTick& tick) noexcept {
    ++stats_.events_received;

    const auto market_result = risk_.validate_market(tick);
    if (market_result != RiskRejectReason::None) {
        record_rejection(market_result, false);
        return std::nullopt;
    }
    if (faulted_) {
        record_rejection(RiskRejectReason::ArithmeticOverflow, false);
        return std::nullopt;
    }

    const Price mark_price =
        tick.bid_ticks + ((tick.ask_ticks - tick.bid_ticks) / 2);
    if (!mark_to_market(mark_price)) {
        enter_arithmetic_fault(false);
        return std::nullopt;
    }
    ++stats_.events_accepted;

    const auto decision = strategy_.on_tick(tick, ledger_.position);
    if (!decision.has_value()) {
        return std::nullopt;
    }
    ++stats_.strategy_signals;

    const auto order_result =
        risk_.check_order(*decision, tick, ledger_);
    if (order_result != RiskRejectReason::None) {
        record_rejection(order_result, true);
        return std::nullopt;
    }

    const Fill fill{
        next_order_id_,
        tick.sequence,
        tick.timestamp_ns,
        decision->side,
        decision->quantity,
        decision->side == Side::Buy ? tick.ask_ticks : tick.bid_ticks,
        decision->score,
    };
    const auto ledger_before_fill = ledger_;
    const auto cost_before_fill = position_cost_ticks_;
    if (!apply_fill(fill) || !mark_to_market(mark_price)) {
        ledger_ = ledger_before_fill;
        position_cost_ticks_ = cost_before_fill;
        enter_arithmetic_fault(true);
        return std::nullopt;
    }

    strategy_.on_order_accepted();
    ++next_order_id_;
    ++stats_.orders_accepted;
    ++stats_.fills;
    return fill;
}

void TradingEngine::set_external_bias(const double value) noexcept {
    strategy_.set_external_bias(value);
}

void TradingEngine::set_kill_switch(const bool enabled) noexcept {
    risk_.set_kill_switch(enabled);
}

void TradingEngine::record_rejection(
    const RiskRejectReason reason, const bool order_rejection) noexcept {
    if (order_rejection) {
        ++stats_.orders_rejected;
    } else {
        ++stats_.events_rejected;
    }
    const auto index = static_cast<std::size_t>(reason);
    if (index < stats_.rejection_counts.size()) {
        ++stats_.rejection_counts[index];
    }
}

void TradingEngine::enter_arithmetic_fault(
    const bool order_rejection) noexcept {
    faulted_ = true;
    risk_.set_kill_switch(true);
    record_rejection(RiskRejectReason::ArithmeticOverflow,
                     order_rejection);
}

bool TradingEngine::apply_fill(const Fill& fill) noexcept {
    LedgerSnapshot next = ledger_;
    std::int64_t next_position_cost = position_cost_ticks_;

    const Quantity signed_quantity =
        fill.side == Side::Buy ? fill.quantity : -fill.quantity;
    const Quantity old_position = next.position;
    const Quantity new_position = old_position + signed_quantity;

    std::int64_t notional{};
    if (!checked_multiply(fill.price_ticks, fill.quantity, notional)) {
        return false;
    }
    const auto cash_delta =
        fill.side == Side::Buy ? -notional : notional;
    if (!checked_add(next.cash_flow_ticks, cash_delta,
                     next.cash_flow_ticks)) {
        return false;
    }

    const bool adding =
        old_position == 0 ||
        (old_position > 0 && signed_quantity > 0) ||
        (old_position < 0 && signed_quantity < 0);

    if (adding) {
        const auto cost_delta =
            fill.side == Side::Buy ? notional : -notional;
        if (!checked_add(next_position_cost, cost_delta,
                         next_position_cost)) {
            return false;
        }
    } else {
        const Quantity old_absolute =
            old_position < 0 ? -old_position : old_position;
        const Quantity closing_quantity =
            std::min(old_absolute, fill.quantity);

        std::int64_t allocated_cost = next_position_cost;
        if (closing_quantity != old_absolute &&
            !checked_multiply(next_position_cost / old_absolute,
                              closing_quantity, allocated_cost)) {
            return false;
        }

        std::int64_t closing_notional{};
        if (!checked_multiply(fill.price_ticks, closing_quantity,
                              closing_notional)) {
            return false;
        }
        const auto closing_cash =
            old_position > 0 ? closing_notional : -closing_notional;
        std::int64_t realized_delta{};
        if (!checked_subtract(closing_cash, allocated_cost,
                              realized_delta) ||
            !checked_add(next.realized_pnl_ticks, realized_delta,
                         next.realized_pnl_ticks) ||
            !checked_subtract(next_position_cost, allocated_cost,
                              next_position_cost)) {
            return false;
        }

        const Quantity reversal_quantity =
            fill.quantity - closing_quantity;
        if (reversal_quantity > 0) {
            std::int64_t reversal_notional{};
            if (!checked_multiply(fill.price_ticks, reversal_quantity,
                                  reversal_notional)) {
                return false;
            }
            const auto reversal_cost =
                fill.side == Side::Buy ? reversal_notional
                                       : -reversal_notional;
            if (!checked_add(next_position_cost, reversal_cost,
                             next_position_cost)) {
                return false;
            }
        }
    }

    next.position = new_position;
    if (new_position == 0) {
        next_position_cost = 0;
        next.average_entry_price_ticks = 0;
    } else {
        if (next_position_cost ==
            std::numeric_limits<std::int64_t>::min()) {
            return false;
        }
        const Quantity absolute_position =
            new_position < 0 ? -new_position : new_position;
        const auto absolute_cost =
            next_position_cost < 0 ? -next_position_cost
                                   : next_position_cost;
        next.average_entry_price_ticks =
            absolute_cost / absolute_position;
    }

    ledger_ = next;
    position_cost_ticks_ = next_position_cost;
    return true;
}

bool TradingEngine::mark_to_market(const Price mark_price) noexcept {
    LedgerSnapshot next = ledger_;
    std::int64_t position_value{};
    if (!checked_multiply(mark_price, next.position,
                          position_value)) {
        return false;
    }

    std::int64_t unrealized{};
    std::int64_t total{};
    if (!checked_subtract(position_value, position_cost_ticks_,
                          unrealized) ||
        !checked_add(next.cash_flow_ticks, position_value, total)) {
        return false;
    }

    next.mark_price_ticks = mark_price;
    next.unrealized_pnl_ticks = unrealized;
    next.total_pnl_ticks = total;
    ledger_ = next;
    return true;
}

}  // namespace stockagent
