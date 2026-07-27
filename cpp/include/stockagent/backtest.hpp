#pragma once

#include "stockagent/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace stockagent {

enum class ResearchStrategy : std::uint8_t {
    Trend,
    MeanReversion,
    OrderBookImbalance,
    Ensemble,
    BuyAndHold,
    NoTrade,
};

[[nodiscard]] constexpr std::string_view to_string(
    const ResearchStrategy strategy) noexcept {
    switch (strategy) {
        case ResearchStrategy::Trend:
            return "trend";
        case ResearchStrategy::MeanReversion:
            return "mean_reversion";
        case ResearchStrategy::OrderBookImbalance:
            return "order_book_imbalance";
        case ResearchStrategy::Ensemble:
            return "ensemble";
        case ResearchStrategy::BuyAndHold:
            return "buy_and_hold";
        case ResearchStrategy::NoTrade:
            return "no_trade";
    }
    return "unknown";
}

struct ResearchSignalConfig {
    ResearchStrategy strategy{ResearchStrategy::Ensemble};
    std::size_t fast_window{8};
    std::size_t slow_window{48};
    std::size_t mean_reversion_window{48};
    std::size_t volatility_window{64};
    std::size_t imbalance_window{8};
    double trend_weight{0.45};
    double mean_reversion_weight{0.25};
    double imbalance_weight{0.30};
    double volatility_target_ticks{4.0};
    double entry_threshold{0.30};
    double exit_threshold{0.10};
    double mean_reversion_trend_filter{0.80};
    Quantity base_target_quantity{40};
    Quantity max_absolute_position{100};
    Quantity max_order_quantity{25};
    std::uint64_t rebalance_interval_events{4};
    std::uint64_t max_holding_events{512};
};

struct ExecutionCostConfig {
    std::int64_t fixed_fee_ticks{2};
    std::int64_t fee_ticks_per_unit{1};
    Price base_slippage_ticks{1};
    double impact_coefficient_ticks{4.0};
    std::uint32_t max_participation_bps{1'000};
    TimestampNs latency_ns{1'000};
};

struct BacktestConfig {
    ResearchSignalConfig signal{};
    ExecutionCostConfig execution{};
    std::size_t warmup_events{};
    std::size_t metric_bucket_events{64};
    std::size_t liquidation_tail_events{32};
};

struct TradeRecord {
    Sequence decision_sequence{};
    Sequence fill_sequence{};
    TimestampNs decision_timestamp_ns{};
    TimestampNs fill_timestamp_ns{};
    Side side{Side::Buy};
    Quantity quantity{};
    Price decision_mid_ticks{};
    Price reference_bbo_ticks{};
    Price fill_price_ticks{};
    long double fee_ticks{};
    long double spread_cost_ticks{};
    long double slippage_cost_ticks{};
    long double latency_cost_ticks{};
    bool forced_liquidation{false};
};

struct EquityPoint {
    Sequence sequence{};
    TimestampNs timestamp_ns{};
    long double gross_liquidation_pnl_ticks{};
    long double net_liquidation_pnl_ticks{};
    Quantity position{};
};

struct BacktestMetrics {
    long double gross_liquidation_pnl_ticks{};
    long double net_liquidation_pnl_ticks{};
    long double executed_fill_shortfall_ticks{};
    long double modeled_total_shortfall_ticks{};
    long double forced_liquidation_fill_shortfall_ticks{};
    long double residual_modeled_liquidation_cost_ticks{};
    long double fees_ticks{};
    long double spread_cost_ticks{};
    long double slippage_cost_ticks{};
    long double latency_cost_ticks{};
    long double max_drawdown_ticks{};
    std::uint64_t max_drawdown_duration_events{};
    long double mean_bucket_pnl_ticks{};
    long double bucket_volatility_ticks{};
    long double downside_deviation_ticks{};
    long double sharpe_per_sqrt_bucket{};
    long double sortino_per_sqrt_bucket{};
    long double mean_worst_5pct_bucket_pnl_ticks{};
    long double worst_bucket_pnl_ticks{};
    long double quantity_turnover{};
    long double average_absolute_exposure{};
    long double time_in_market_ratio{};
    long double closed_trade_hit_rate{};
    long double closed_trade_profit_factor{};
    long double average_closed_trade_pnl_ticks{};
    Quantity ending_position{};
    Quantity maximum_absolute_position{};
    std::uint64_t events{};
    std::uint64_t metric_buckets{};
    std::uint64_t intents_submitted{};
    std::uint64_t intents_replaced{};
    std::uint64_t fills{};
    std::uint64_t partial_or_capacity_limited_fills{};
    std::uint64_t cancelled_unfilled_intents{};
    std::uint64_t closed_trades{};
    std::uint64_t winning_closed_trades{};
    bool sharpe_available{false};
    bool sortino_available{false};
    bool tail_metric_available{false};
    bool all_liquidation_marks_available{false};
    bool terminal_liquidation_complete{false};
};

struct BacktestResult {
    BacktestMetrics metrics{};
    std::vector<TradeRecord> trades;
    std::vector<EquityPoint> equity_curve;
    std::vector<long double> bucket_pnl_ticks;
    std::vector<long double> closed_trade_pnl_ticks;
};

class ResearchSignalModel {
  public:
    explicit ResearchSignalModel(ResearchSignalConfig config);

    [[nodiscard]] std::optional<Quantity> on_tick(
        const MarketTick& tick, Quantity current_position);

    [[nodiscard]] std::size_t required_warmup_events() const noexcept;
    [[nodiscard]] double last_score() const noexcept { return last_score_; }
    [[nodiscard]] double last_volatility_ticks() const noexcept {
        return last_volatility_ticks_;
    }

  private:
    ResearchSignalConfig config_;
    std::vector<double> mean_prices_;
    std::size_t mean_cursor_{};
    std::size_t mean_count_{};
    long double mean_sum_{};
    long double mean_square_sum_{};
    double fast_ema_{};
    double slow_ema_{};
    double return_variance_ewma_{};
    double imbalance_ewma_{};
    double previous_mid_{};
    double last_score_{};
    double last_volatility_ticks_{1.0};
    std::uint64_t event_count_{};
    std::uint64_t holding_events_{};
    bool initialized_{false};
};

struct WalkForwardConfig {
    std::size_t folds{4};
    std::size_t final_test_percent{20};
    std::size_t embargo_events{64};
    std::uint64_t minimum_closed_trades{2};
};

struct WalkForwardFold {
    std::size_t training_begin{};
    std::size_t training_end{};
    std::size_t validation_begin{};
    std::size_t validation_end{};
    long double training_score{};
    BacktestMetrics training_metrics{};
    BacktestMetrics validation_metrics{};
};

struct WalkForwardReport {
    BacktestConfig best_catalog_config{};
    BacktestConfig recommended_outcome_config{};
    BacktestConfig cost_latency_scenario_config{};
    BacktestConfig buy_and_hold_config{};
    WalkForwardConfig walk_forward_config{};
    std::vector<WalkForwardFold> best_catalog_candidate_folds;
    BacktestMetrics best_catalog_aggregate_validation_metrics{};
    BacktestMetrics best_catalog_final_test_metrics{};
    BacktestMetrics recommended_outcome_final_test_metrics{};
    BacktestMetrics cost_latency_scenario_metrics{};
    BacktestMetrics buy_and_hold_final_test_metrics{};
    std::size_t final_test_begin{};
    std::size_t candidates{};
    std::size_t candidate_fold_trials{};
    std::size_t positive_net_pnl_validation_folds{};
    long double selection_median_score{};
    long double selection_worst_fold_score{};
    long double selection_worst_fold_pnl_ticks{};
    std::uint64_t data_fingerprint{};
    bool best_catalog_candidate_accepted{false};
};

[[nodiscard]] BacktestResult run_backtest(
    const std::vector<MarketTick>& ticks, std::size_t begin,
    std::size_t end, const BacktestConfig& config);

[[nodiscard]] long double score_backtest(
    const BacktestMetrics& metrics,
    std::uint64_t minimum_closed_trades) noexcept;

[[nodiscard]] std::vector<BacktestConfig> default_backtest_candidates(
    const ExecutionCostConfig& execution = {});

[[nodiscard]] WalkForwardReport run_walk_forward(
    const std::vector<MarketTick>& ticks,
    const std::vector<BacktestConfig>& candidates,
    const WalkForwardConfig& config = {});

[[nodiscard]] std::uint64_t fingerprint_market_data(
    const std::vector<MarketTick>& ticks) noexcept;

}  // namespace stockagent
