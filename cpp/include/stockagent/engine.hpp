#pragma once

#include "stockagent/risk.hpp"
#include "stockagent/strategy.hpp"
#include "stockagent/types.hpp"

#include <optional>

namespace stockagent {

struct EngineConfig {
    StrategyConfig strategy{};
    RiskConfig risk{};
};

class TradingEngine {
  public:
    explicit TradingEngine(EngineConfig config = {});

    [[nodiscard]] std::optional<Fill> on_market_tick(
        const MarketTick& tick) noexcept;

    void set_external_bias(double value) noexcept;
    void set_kill_switch(bool enabled) noexcept;

    [[nodiscard]] const LedgerSnapshot& ledger() const noexcept {
        return ledger_;
    }

    [[nodiscard]] const EngineStats& stats() const noexcept { return stats_; }

    [[nodiscard]] double last_signal_score() const noexcept {
        return strategy_.last_score();
    }

    [[nodiscard]] bool faulted() const noexcept { return faulted_; }

  private:
    void record_rejection(RiskRejectReason reason,
                          bool order_rejection) noexcept;
    void enter_arithmetic_fault(bool order_rejection) noexcept;
    [[nodiscard]] bool apply_fill(const Fill& fill) noexcept;
    [[nodiscard]] bool mark_to_market(Price mark_price) noexcept;

    SignalStrategy strategy_;
    RiskManager risk_;
    LedgerSnapshot ledger_{};
    EngineStats stats_{};
    std::int64_t position_cost_ticks_{};
    std::uint64_t next_order_id_{1};
    bool faulted_{false};
};

}  // namespace stockagent
