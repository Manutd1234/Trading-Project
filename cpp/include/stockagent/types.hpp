#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace stockagent {

using Price = std::int64_t;
using Quantity = std::int64_t;
using TimestampNs = std::uint64_t;
using Sequence = std::uint64_t;

enum class Side : std::uint8_t {
    Buy,
    Sell,
};

[[nodiscard]] constexpr std::string_view to_string(const Side side) noexcept {
    return side == Side::Buy ? "BUY" : "SELL";
}

struct MarketTick {
    Sequence sequence{};
    TimestampNs timestamp_ns{};
    Price bid_ticks{};
    Quantity bid_quantity{};
    Price ask_ticks{};
    Quantity ask_quantity{};
};

struct StrategyDecision {
    Side side{Side::Buy};
    Quantity quantity{};
    double score{};
};

struct Fill {
    std::uint64_t order_id{};
    Sequence market_sequence{};
    TimestampNs timestamp_ns{};
    Side side{Side::Buy};
    Quantity quantity{};
    Price price_ticks{};
    double signal_score{};
};

enum class RiskRejectReason : std::uint8_t {
    None,
    BadQuote,
    NonMonotonicEvent,
    StaleEvent,
    KillSwitch,
    InvalidOrder,
    OrderSize,
    InsufficientLiquidity,
    PositionLimit,
    NotionalLimit,
    LossLimit,
    ArithmeticOverflow,
    Count,
};

inline constexpr std::size_t risk_reject_reason_count =
    static_cast<std::size_t>(RiskRejectReason::Count);

[[nodiscard]] constexpr std::string_view to_string(
    const RiskRejectReason reason) noexcept {
    switch (reason) {
        case RiskRejectReason::None:
            return "none";
        case RiskRejectReason::BadQuote:
            return "bad_quote";
        case RiskRejectReason::NonMonotonicEvent:
            return "non_monotonic_event";
        case RiskRejectReason::StaleEvent:
            return "stale_event";
        case RiskRejectReason::KillSwitch:
            return "kill_switch";
        case RiskRejectReason::InvalidOrder:
            return "invalid_order";
        case RiskRejectReason::OrderSize:
            return "order_size";
        case RiskRejectReason::InsufficientLiquidity:
            return "insufficient_liquidity";
        case RiskRejectReason::PositionLimit:
            return "position_limit";
        case RiskRejectReason::NotionalLimit:
            return "notional_limit";
        case RiskRejectReason::LossLimit:
            return "loss_limit";
        case RiskRejectReason::ArithmeticOverflow:
            return "arithmetic_overflow";
        case RiskRejectReason::Count:
            return "count";
    }
    return "unknown";
}

struct LedgerSnapshot {
    Quantity position{};
    Price average_entry_price_ticks{};
    Price mark_price_ticks{};
    std::int64_t cash_flow_ticks{};
    std::int64_t realized_pnl_ticks{};
    std::int64_t unrealized_pnl_ticks{};
    std::int64_t total_pnl_ticks{};
};

struct EngineStats {
    std::uint64_t events_received{};
    std::uint64_t events_accepted{};
    std::uint64_t events_rejected{};
    std::uint64_t strategy_signals{};
    std::uint64_t orders_accepted{};
    std::uint64_t orders_rejected{};
    std::uint64_t fills{};
    std::array<std::uint64_t, risk_reject_reason_count> rejection_counts{};
};

}  // namespace stockagent
