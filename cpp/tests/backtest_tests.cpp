#include "stockagent/backtest.hpp"
#include "stockagent/synthetic_feed.hpp"
#include "stockagent/types.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void check(const bool condition, const std::string_view expression,
           const std::string_view test_name, const int line) {
    if (condition) {
        return;
    }
    ++failures;
    std::cerr << "FAIL " << test_name << ':' << line << " check("
              << expression << ")\n";
}

#define CHECK(test_name, expression) \
    check((expression), #expression, (test_name), __LINE__)

[[nodiscard]] stockagent::MarketTick make_tick(
    const std::uint64_t sequence,
    const stockagent::Price mid,
    const stockagent::Quantity bid_quantity = 500,
    const stockagent::Quantity ask_quantity = 500) {
    return stockagent::MarketTick{
        sequence,
        sequence * 1'000U,
        mid - 5,
        bid_quantity,
        mid + 5,
        ask_quantity,
    };
}

[[nodiscard]] std::vector<stockagent::MarketTick>
make_trending_ticks(const std::size_t count,
                    const stockagent::Price step = 3) {
    std::vector<stockagent::MarketTick> ticks;
    ticks.reserve(count);
    stockagent::Price mid = 100'000;
    for (std::size_t index = 0; index < count; ++index) {
        mid += step;
        ticks.push_back(
            make_tick(
                static_cast<std::uint64_t>(index + 1U),
                mid, 700, 300));
    }
    return ticks;
}

[[nodiscard]] stockagent::BacktestConfig buy_hold_config() {
    stockagent::BacktestConfig config;
    config.signal.strategy =
        stockagent::ResearchStrategy::BuyAndHold;
    config.signal.fast_window = 2U;
    config.signal.slow_window = 4U;
    config.signal.mean_reversion_window = 4U;
    config.signal.volatility_window = 4U;
    config.signal.imbalance_window = 2U;
    config.signal.base_target_quantity = 20;
    config.signal.max_absolute_position = 100;
    config.signal.max_order_quantity = 100;
    config.signal.rebalance_interval_events = 1U;
    config.execution.fixed_fee_ticks = 0;
    config.execution.fee_ticks_per_unit = 0;
    config.execution.base_slippage_ticks = 0;
    config.execution.impact_coefficient_ticks = 0.0;
    config.execution.max_participation_bps = 10'000U;
    config.execution.latency_ns = 0U;
    config.metric_bucket_events = 4U;
    config.liquidation_tail_events = 4U;
    return config;
}

void test_public_signal_families() {
    constexpr std::string_view name = "public_signal_families";

    stockagent::ResearchSignalConfig trend;
    trend.strategy = stockagent::ResearchStrategy::Trend;
    trend.fast_window = 2U;
    trend.slow_window = 6U;
    trend.mean_reversion_window = 4U;
    trend.volatility_window = 4U;
    trend.imbalance_window = 2U;
    trend.entry_threshold = 0.10;
    trend.exit_threshold = 0.02;
    trend.rebalance_interval_events = 1U;
    stockagent::ResearchSignalModel trend_model(trend);
    std::optional<stockagent::Quantity> trend_target;
    for (std::uint64_t sequence = 1U;
         sequence <= 12U; ++sequence) {
        trend_target = trend_model.on_tick(
            make_tick(
                sequence,
                100'000 +
                    static_cast<stockagent::Price>(
                        sequence * 10U)),
            0);
    }
    CHECK(name, trend_model.last_score() > 0.0);
    CHECK(name, trend_target.has_value());
    CHECK(name, *trend_target > 0);

    stockagent::ResearchSignalConfig mean = trend;
    mean.strategy =
        stockagent::ResearchStrategy::MeanReversion;
    mean.slow_window = 8U;
    mean.mean_reversion_window = 5U;
    mean.mean_reversion_trend_filter = 1.0;
    stockagent::ResearchSignalModel mean_model(mean);
    for (std::uint64_t sequence = 1U;
         sequence <= 10U; ++sequence) {
        static_cast<void>(
            mean_model.on_tick(
                make_tick(sequence, 100'000), 0));
    }
    const auto mean_target =
        mean_model.on_tick(
            make_tick(11U, 100'200), 0);
    CHECK(name, mean_model.last_score() < 0.0);
    CHECK(name, mean_target.has_value());
    CHECK(name, *mean_target < 0);

    stockagent::ResearchSignalConfig imbalance = trend;
    imbalance.strategy =
        stockagent::ResearchStrategy::OrderBookImbalance;
    imbalance.entry_threshold = 0.10;
    stockagent::ResearchSignalModel imbalance_model(imbalance);
    std::optional<stockagent::Quantity> imbalance_target;
    for (std::uint64_t sequence = 1U;
         sequence <= 6U; ++sequence) {
        imbalance_target =
            imbalance_model.on_tick(
                make_tick(sequence, 100'000, 900, 100), 0);
    }
    CHECK(name, imbalance_model.last_score() > 0.0);
    CHECK(name, imbalance_target.has_value());
    CHECK(name, *imbalance_target > 0);
}

void test_causal_fill_and_latency() {
    constexpr std::string_view name = "causal_fill_and_latency";
    const auto ticks = make_trending_ticks(20U);

    auto immediate = buy_hold_config();
    const auto immediate_result =
        stockagent::run_backtest(
            ticks, 0U, ticks.size(), immediate);
    CHECK(name, !immediate_result.trades.empty());
    const auto& first = immediate_result.trades.front();
    CHECK(name, first.fill_sequence > first.decision_sequence);
    CHECK(name,
          first.fill_timestamp_ns >
              first.decision_timestamp_ns);

    auto delayed = immediate;
    delayed.execution.latency_ns = 3'000U;
    const auto delayed_result =
        stockagent::run_backtest(
            ticks, 0U, ticks.size(), delayed);
    CHECK(name, !delayed_result.trades.empty());
    const auto& delayed_first =
        delayed_result.trades.front();
    CHECK(name,
          delayed_first.fill_timestamp_ns -
                  delayed_first.decision_timestamp_ns >=
              3'000U);
    CHECK(name,
          delayed_first.fill_sequence >
              first.fill_sequence);
}

void test_future_quotes_change_fill_not_decision() {
    constexpr std::string_view name =
        "future_quotes_change_fill_not_decision";
    auto first_ticks = make_trending_ticks(20U, 0);
    auto second_ticks = first_ticks;
    second_ticks[1].bid_ticks += 100;
    second_ticks[1].ask_ticks += 100;

    const auto config = buy_hold_config();
    const auto first =
        stockagent::run_backtest(
            first_ticks, 0U, first_ticks.size(), config);
    const auto second =
        stockagent::run_backtest(
            second_ticks, 0U, second_ticks.size(), config);
    CHECK(name, !first.trades.empty());
    CHECK(name, !second.trades.empty());
    CHECK(name,
          first.trades.front().decision_sequence ==
              second.trades.front().decision_sequence);
    CHECK(name,
          first.trades.front().decision_mid_ticks ==
              second.trades.front().decision_mid_ticks);
    CHECK(name,
          first.trades.front().fill_price_ticks !=
              second.trades.front().fill_price_ticks);
}

void test_cost_monotonicity_and_decomposition() {
    constexpr std::string_view name =
        "cost_monotonicity_and_decomposition";
    const auto ticks = make_trending_ticks(100U);
    const auto free_config = buy_hold_config();
    const auto free_result =
        stockagent::run_backtest(
            ticks, 0U, ticks.size(), free_config);

    auto costly_config = free_config;
    costly_config.execution.fixed_fee_ticks = 10;
    costly_config.execution.fee_ticks_per_unit = 2;
    costly_config.execution.base_slippage_ticks = 3;
    costly_config.execution.impact_coefficient_ticks = 8.0;
    const auto costly_result =
        stockagent::run_backtest(
            ticks, 0U, ticks.size(), costly_config);

    CHECK(name,
          costly_result.metrics.gross_liquidation_pnl_ticks ==
              free_result.metrics.gross_liquidation_pnl_ticks);
    CHECK(name,
          costly_result.metrics.net_liquidation_pnl_ticks <
              free_result.metrics.net_liquidation_pnl_ticks);
    CHECK(name,
          costly_result.metrics.executed_fill_shortfall_ticks >
              free_result.metrics.executed_fill_shortfall_ticks);
    CHECK(name, costly_result.metrics.fees_ticks > 0.0L);
    CHECK(name,
          costly_result.metrics.slippage_cost_ticks > 0.0L);
    CHECK(name,
          std::abs(
              (costly_result.metrics
                   .gross_liquidation_pnl_ticks -
               costly_result.metrics
                   .net_liquidation_pnl_ticks) -
              costly_result.metrics
                  .modeled_total_shortfall_ticks) <
              1.0e-9L);
    CHECK(name,
          std::abs(
              costly_result.metrics
                  .executed_fill_shortfall_ticks -
              (costly_result.metrics.fees_ticks +
               costly_result.metrics.spread_cost_ticks +
               costly_result.metrics.slippage_cost_ticks +
               costly_result.metrics.latency_cost_ticks)) <
              1.0e-9L);
    CHECK(name,
          std::abs(
              costly_result.metrics
                  .modeled_total_shortfall_ticks -
              (costly_result.metrics
                   .executed_fill_shortfall_ticks +
               costly_result.metrics
                   .residual_modeled_liquidation_cost_ticks)) <
              1.0e-9L);
    CHECK(name,
          costly_result.metrics
                  .residual_modeled_liquidation_cost_ticks ==
              0.0L);
    CHECK(name,
          costly_result.metrics
                  .forced_liquidation_fill_shortfall_ticks >
              0.0L);
    long double forced_fill_shortfall{};
    for (const auto& trade : costly_result.trades) {
        if (trade.forced_liquidation) {
            forced_fill_shortfall +=
                trade.fee_ticks +
                trade.spread_cost_ticks +
                trade.slippage_cost_ticks +
                trade.latency_cost_ticks;
        }
    }
    CHECK(name,
          std::abs(
              forced_fill_shortfall -
              costly_result.metrics
                  .forced_liquidation_fill_shortfall_ticks) <
              1.0e-9L);
    CHECK(name, costly_result.metrics.ending_position == 0);
    CHECK(name,
          costly_result.metrics.terminal_liquidation_complete);
}

void test_participation_cap_and_position_limit() {
    constexpr std::string_view name =
        "participation_cap_and_position_limit";
    auto ticks = make_trending_ticks(30U);
    for (auto& tick : ticks) {
        tick.bid_quantity = 50;
        tick.ask_quantity = 50;
    }
    auto config = buy_hold_config();
    config.signal.base_target_quantity = 100;
    config.signal.max_absolute_position = 100;
    config.signal.max_order_quantity = 100;
    config.execution.max_participation_bps = 1'000U;

    const auto result =
        stockagent::run_backtest(
            ticks, 0U, ticks.size(), config);
    CHECK(name, !result.trades.empty());
    for (const auto& trade : result.trades) {
        CHECK(name, trade.quantity <= 5);
    }
    CHECK(name,
          result.metrics
                  .partial_or_capacity_limited_fills >
              0U);
    CHECK(name,
          result.metrics.maximum_absolute_position <=
              config.signal.max_absolute_position);
}

void test_multi_slice_forced_liquidation() {
    constexpr std::string_view name =
        "multi_slice_forced_liquidation";
    const auto ticks = make_trending_ticks(40U);
    auto config = buy_hold_config();
    config.signal.base_target_quantity = 20;
    config.signal.max_absolute_position = 20;
    config.signal.max_order_quantity = 5;
    config.liquidation_tail_events = 8U;

    const auto result =
        stockagent::run_backtest(
            ticks, 0U, ticks.size(), config);
    std::uint64_t forced_fills{};
    for (const auto& trade : result.trades) {
        if (trade.forced_liquidation) {
            ++forced_fills;
            CHECK(name, trade.side == stockagent::Side::Sell);
            CHECK(name, trade.quantity == 5);
        }
    }
    CHECK(name, forced_fills == 4U);
    CHECK(name,
          result.metrics
                  .forced_liquidation_fill_shortfall_ticks >
              0.0L);
    CHECK(name, result.metrics.ending_position == 0);
    CHECK(name, result.metrics.terminal_liquidation_complete);
}

void test_short_side_cost_identity() {
    constexpr std::string_view name =
        "short_side_cost_identity";
    std::vector<stockagent::MarketTick> ticks;
    ticks.reserve(40U);
    for (std::uint64_t sequence = 1U;
         sequence <= 40U; ++sequence) {
        ticks.push_back(
            make_tick(sequence, 100'000, 100, 900));
    }
    auto config = buy_hold_config();
    config.signal.strategy =
        stockagent::ResearchStrategy::OrderBookImbalance;
    config.signal.fast_window = 2U;
    config.signal.slow_window = 4U;
    config.signal.mean_reversion_window = 4U;
    config.signal.volatility_window = 4U;
    config.signal.imbalance_window = 1U;
    config.signal.entry_threshold = 0.10;
    config.signal.exit_threshold = 0.02;
    config.signal.base_target_quantity = 20;
    config.signal.max_absolute_position = 20;
    config.signal.max_order_quantity = 20;
    config.signal.rebalance_interval_events = 1U;
    config.execution.fixed_fee_ticks = 2;
    config.execution.fee_ticks_per_unit = 1;
    config.execution.base_slippage_ticks = 1;
    config.execution.impact_coefficient_ticks = 4.0;
    config.liquidation_tail_events = 6U;

    const auto result =
        stockagent::run_backtest(
            ticks, 0U, ticks.size(), config);
    CHECK(name, !result.trades.empty());
    CHECK(name,
          result.trades.front().side ==
              stockagent::Side::Sell);
    CHECK(name,
          result.trades.back().side ==
              stockagent::Side::Buy);
    CHECK(name, result.trades.back().forced_liquidation);
    CHECK(name, result.metrics.ending_position == 0);
    CHECK(name, result.metrics.terminal_liquidation_complete);
    CHECK(name,
          std::abs(
              result.metrics.executed_fill_shortfall_ticks -
              (result.metrics.fees_ticks +
               result.metrics.spread_cost_ticks +
               result.metrics.slippage_cost_ticks +
               result.metrics.latency_cost_ticks)) <
              1.0e-9L);
    CHECK(name,
          std::abs(
              (result.metrics.gross_liquidation_pnl_ticks -
               result.metrics.net_liquidation_pnl_ticks) -
              result.metrics.modeled_total_shortfall_ticks) <
              1.0e-9L);
}

void test_participation_floor_can_produce_no_fill() {
    constexpr std::string_view name =
        "participation_floor_can_produce_no_fill";
    auto ticks = make_trending_ticks(20U);
    for (auto& tick : ticks) {
        tick.bid_quantity = 9;
        tick.ask_quantity = 9;
    }
    auto config = buy_hold_config();
    config.execution.max_participation_bps = 1'000U;

    const auto result =
        stockagent::run_backtest(
            ticks, 0U, ticks.size(), config);
    CHECK(name, result.trades.empty());
    CHECK(name, result.metrics.fills == 0U);
    CHECK(name, result.metrics.ending_position == 0);
    CHECK(name, result.metrics.terminal_liquidation_complete);
}

void test_incomplete_causal_liquidation_is_ineligible() {
    constexpr std::string_view name =
        "incomplete_causal_liquidation_is_ineligible";
    auto ticks = make_trending_ticks(20U);
    for (std::size_t index = 16U;
         index < ticks.size(); ++index) {
        ticks[index].bid_quantity = 9;
        ticks[index].ask_quantity = 9;
    }
    auto config = buy_hold_config();
    config.execution.max_participation_bps = 1'000U;

    const auto result =
        stockagent::run_backtest(
            ticks, 0U, ticks.size(), config);
    CHECK(name, !result.trades.empty());
    CHECK(name, result.metrics.ending_position != 0);
    CHECK(name, !result.metrics.terminal_liquidation_complete);
    CHECK(name, !result.metrics.all_liquidation_marks_available);
    CHECK(name,
          result.metrics
                  .residual_modeled_liquidation_cost_ticks >
              0.0L);
    CHECK(name,
          std::abs(
              result.metrics.modeled_total_shortfall_ticks -
              (result.metrics.executed_fill_shortfall_ticks +
               result.metrics
                   .residual_modeled_liquidation_cost_ticks)) <
              1.0e-9L);
    CHECK(name,
          !std::isfinite(
              stockagent::score_backtest(
                  result.metrics, 1U)));
}

void test_liquidation_tail_cancels_risk_seeking_pending_intent() {
    constexpr std::string_view name =
        "liquidation_tail_cancels_risk_seeking_pending_intent";
    const auto ticks = make_trending_ticks(20U);
    auto config = buy_hold_config();
    config.execution.latency_ns = 16'000U;

    const auto result =
        stockagent::run_backtest(
            ticks, 0U, ticks.size(), config);
    CHECK(name, result.trades.empty());
    CHECK(name, result.metrics.fills == 0U);
    CHECK(name, result.metrics.intents_replaced > 0U);
    CHECK(name, result.metrics.ending_position == 0);
    CHECK(name, result.metrics.terminal_liquidation_complete);
}

void test_unavailable_path_liquidation_mark_is_ineligible() {
    constexpr std::string_view name =
        "unavailable_path_liquidation_mark_is_ineligible";
    auto ticks = make_trending_ticks(30U);
    ticks[5].bid_quantity = 9;
    ticks[5].ask_quantity = 9;
    auto config = buy_hold_config();
    config.execution.max_participation_bps = 1'000U;

    const auto result =
        stockagent::run_backtest(
            ticks, 0U, ticks.size(), config);
    CHECK(name, result.metrics.ending_position == 0);
    CHECK(name, result.metrics.terminal_liquidation_complete);
    CHECK(name, !result.metrics.all_liquidation_marks_available);
    CHECK(name,
          !std::isfinite(
              stockagent::score_backtest(
                  result.metrics, 1U)));
}

void test_score_is_scale_normalized_and_requires_closed_trades() {
    constexpr std::string_view name =
        "score_is_scale_normalized_and_requires_closed_trades";
    stockagent::BacktestMetrics one;
    one.net_liquidation_pnl_ticks = 1'000.0L;
    one.max_drawdown_ticks = 100.0L;
    one.executed_fill_shortfall_ticks = 100.0L;
    one.average_absolute_exposure = 10.0L;
    one.closed_trades = 2U;
    one.ending_position = 0;
    one.all_liquidation_marks_available = true;
    one.terminal_liquidation_complete = true;

    auto two = one;
    two.net_liquidation_pnl_ticks *= 2.0L;
    two.max_drawdown_ticks *= 2.0L;
    two.executed_fill_shortfall_ticks *= 2.0L;
    two.average_absolute_exposure *= 2.0L;
    const auto one_score =
        stockagent::score_backtest(one, 2U);
    const auto two_score =
        stockagent::score_backtest(two, 2U);
    CHECK(name, std::abs(one_score - two_score) < 1.0e-12L);

    auto no_closed_episode = one;
    no_closed_episode.closed_trades = 0U;
    no_closed_episode.fills = 10U;
    CHECK(name,
          !std::isfinite(
              stockagent::score_backtest(
                  no_closed_episode, 1U)));
}

void test_distribution_metric_sample_floors() {
    constexpr std::string_view name =
        "distribution_metric_sample_floors";
    auto config = buy_hold_config();
    config.metric_bucket_events = 5U;

    const auto nineteen =
        stockagent::run_backtest(
            make_trending_ticks(95U), 0U, 95U, config);
    CHECK(name, nineteen.metrics.metric_buckets == 19U);
    CHECK(name, !nineteen.metrics.sharpe_available);
    CHECK(name, !nineteen.metrics.sortino_available);
    CHECK(name, !nineteen.metrics.tail_metric_available);

    const auto nineteen_with_remainder =
        stockagent::run_backtest(
            make_trending_ticks(99U), 0U, 99U, config);
    CHECK(name,
          nineteen_with_remainder.metrics.metric_buckets ==
              19U);
    CHECK(name,
          nineteen_with_remainder.bucket_pnl_ticks.size() ==
              19U);

    const auto twenty =
        stockagent::run_backtest(
            make_trending_ticks(100U), 0U, 100U, config);
    CHECK(name, twenty.metrics.metric_buckets == 20U);
    CHECK(name, twenty.metrics.sharpe_available);
    CHECK(name, twenty.metrics.sortino_available);
    CHECK(name, !twenty.metrics.tail_metric_available);

    const auto forty =
        stockagent::run_backtest(
            make_trending_ticks(200U), 0U, 200U, config);
    CHECK(name, forty.metrics.metric_buckets == 40U);
    CHECK(name, forty.metrics.tail_metric_available);

    const auto all_positive =
        stockagent::run_backtest(
            make_trending_ticks(100U, 100), 0U, 100U, config);
    CHECK(name, all_positive.metrics.sharpe_available);
    CHECK(name, !all_positive.metrics.sortino_available);
}

[[nodiscard]] bool same_signal_config(
    const stockagent::ResearchSignalConfig& left,
    const stockagent::ResearchSignalConfig& right) {
    return left.strategy == right.strategy &&
           left.fast_window == right.fast_window &&
           left.slow_window == right.slow_window &&
           left.mean_reversion_window ==
               right.mean_reversion_window &&
           left.volatility_window ==
               right.volatility_window &&
           left.imbalance_window ==
               right.imbalance_window &&
           left.entry_threshold == right.entry_threshold &&
           left.exit_threshold == right.exit_threshold &&
           left.trend_weight == right.trend_weight &&
           left.mean_reversion_weight ==
               right.mean_reversion_weight &&
           left.imbalance_weight ==
               right.imbalance_weight;
}

void test_walk_forward_final_test_isolation() {
    constexpr std::string_view name =
        "walk_forward_final_test_isolation";
    std::vector<stockagent::MarketTick> ticks;
    ticks.reserve(5'000U);
    stockagent::SyntheticFeed feed(123U);
    for (std::size_t index = 0; index < 5'000U; ++index) {
        ticks.push_back(feed.next());
    }
    stockagent::WalkForwardConfig walk;
    walk.folds = 3U;
    walk.final_test_percent = 20U;
    walk.embargo_events = 32U;
    walk.minimum_closed_trades = 1U;
    const auto candidates =
        stockagent::default_backtest_candidates();
    const auto first =
        stockagent::run_walk_forward(
            ticks, candidates, walk);

    auto changed = ticks;
    for (std::size_t index = first.final_test_begin;
         index < changed.size(); ++index) {
        const auto shift =
            static_cast<stockagent::Price>(
                (index - first.final_test_begin) * 20U);
        changed[index].bid_ticks += shift;
        changed[index].ask_ticks += shift;
    }
    const auto second =
        stockagent::run_walk_forward(
            changed, candidates, walk);

    CHECK(name,
          same_signal_config(
              first.best_catalog_config.signal,
              second.best_catalog_config.signal));
    CHECK(name,
          first.selection_median_score ==
              second.selection_median_score);
    CHECK(name,
          first.data_fingerprint !=
              second.data_fingerprint);
    CHECK(name,
          first.best_catalog_final_test_metrics
                  .net_liquidation_pnl_ticks !=
              second.best_catalog_final_test_metrics
                  .net_liquidation_pnl_ticks);
    for (const auto& fold :
         first.best_catalog_candidate_folds) {
        CHECK(name,
              fold.training_end +
                      walk.embargo_events <=
                  fold.validation_begin);
        CHECK(name,
              fold.validation_end <=
                  first.final_test_begin -
                      walk.embargo_events);
    }
    long double validation_net{};
    long double validation_shortfall{};
    std::uint64_t validation_fills{};
    std::uint64_t validation_closed_trades{};
    for (const auto& fold :
         first.best_catalog_candidate_folds) {
        validation_net +=
            fold.validation_metrics
                .net_liquidation_pnl_ticks;
        validation_shortfall +=
            fold.validation_metrics
                .modeled_total_shortfall_ticks;
        validation_fills +=
            fold.validation_metrics.fills;
        validation_closed_trades +=
            fold.validation_metrics.closed_trades;
    }
    CHECK(name,
          std::abs(
              validation_net -
              first
                  .best_catalog_aggregate_validation_metrics
                  .net_liquidation_pnl_ticks) <
              1.0e-9L);
    CHECK(name,
          std::abs(
              validation_shortfall -
              first
                  .best_catalog_aggregate_validation_metrics
                  .modeled_total_shortfall_ticks) <
              1.0e-9L);
    CHECK(name,
          validation_fills ==
              first
                  .best_catalog_aggregate_validation_metrics
                  .fills);
    CHECK(name,
          validation_closed_trades ==
              first
                  .best_catalog_aggregate_validation_metrics
                  .closed_trades);
}

void test_walk_forward_reports_no_trade_gate() {
    constexpr std::string_view name =
        "walk_forward_reports_no_trade_gate";
    std::vector<stockagent::MarketTick> ticks;
    ticks.reserve(5'000U);
    stockagent::SyntheticFeed feed(77U);
    for (std::size_t index = 0; index < 5'000U; ++index) {
        ticks.push_back(feed.next());
    }
    stockagent::ExecutionCostConfig execution;
    execution.fixed_fee_ticks = 100'000;
    auto candidates =
        stockagent::default_backtest_candidates(execution);
    candidates.resize(1U);
    stockagent::WalkForwardConfig walk;
    walk.folds = 3U;
    walk.final_test_percent = 20U;
    walk.embargo_events = 32U;
    walk.minimum_closed_trades = 1U;

    const auto report =
        stockagent::run_walk_forward(
            ticks, candidates, walk);
    CHECK(name, report.selection_median_score < 0.0L);
    CHECK(name, !report.best_catalog_candidate_accepted);
    CHECK(name,
          report.recommended_outcome_config.signal.strategy ==
              stockagent::ResearchStrategy::NoTrade);
    CHECK(name,
          report.recommended_outcome_final_test_metrics
                  .net_liquidation_pnl_ticks ==
              0.0L);
    CHECK(name,
          report.recommended_outcome_final_test_metrics.fills ==
              0U);
}

void test_scale_tie_uses_stable_catalog_order() {
    constexpr std::string_view name =
        "scale_tie_uses_stable_catalog_order";
    const auto ticks = make_trending_ticks(5'000U);
    auto one = buy_hold_config();
    one.signal.base_target_quantity = 10;
    one.signal.max_absolute_position = 10;
    one.signal.max_order_quantity = 10;
    auto two = one;
    two.signal.base_target_quantity = 20;
    two.signal.max_absolute_position = 20;
    two.signal.max_order_quantity = 20;
    stockagent::WalkForwardConfig walk;
    walk.folds = 3U;
    walk.final_test_percent = 20U;
    walk.embargo_events = 32U;
    walk.minimum_closed_trades = 1U;

    const auto report =
        stockagent::run_walk_forward(
            ticks, {one, two}, walk);
    CHECK(name,
          report.best_catalog_config.signal
                  .base_target_quantity ==
              one.signal.base_target_quantity);
    CHECK(name,
          report.buy_and_hold_config.signal
                  .base_target_quantity ==
              40);
}

void test_one_nonpositive_fold_recommends_no_trade() {
    constexpr std::string_view name =
        "one_nonpositive_fold_recommends_no_trade";
    std::vector<stockagent::MarketTick> ticks;
    ticks.reserve(5'000U);
    stockagent::Price mid = 100'000;
    for (std::size_t index = 0U;
         index < 5'000U; ++index) {
        const bool losing_validation =
            index >= 2'677U && index < 3'306U;
        mid += losing_validation ? -3 : 3;
        ticks.push_back(
            make_tick(
                static_cast<std::uint64_t>(index + 1U),
                mid));
    }
    auto candidate = buy_hold_config();
    candidate.metric_bucket_events = 64U;
    stockagent::WalkForwardConfig walk;
    walk.folds = 3U;
    walk.final_test_percent = 20U;
    walk.embargo_events = 32U;
    walk.minimum_closed_trades = 1U;

    const auto report =
        stockagent::run_walk_forward(
            ticks, {candidate}, walk);
    CHECK(name, report.selection_median_score > 0.0L);
    CHECK(name, report.selection_worst_fold_score < 0.0L);
    CHECK(name, !report.best_catalog_candidate_accepted);
    CHECK(name,
          report.recommended_outcome_config.signal.strategy ==
              stockagent::ResearchStrategy::NoTrade);
}

void test_walk_forward_requires_common_evaluation_regime() {
    constexpr std::string_view name =
        "walk_forward_requires_common_evaluation_regime";
    const auto ticks = make_trending_ticks(5'000U);
    auto candidates =
        stockagent::default_backtest_candidates();
    candidates.resize(2U);
    candidates[1].execution.fixed_fee_ticks += 1;
    bool threw = false;
    try {
        static_cast<void>(
            stockagent::run_walk_forward(
                ticks, candidates));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(name, threw);

    candidates =
        stockagent::default_backtest_candidates();
    candidates.resize(2U);
    candidates[1].liquidation_tail_events += 1U;
    threw = false;
    try {
        static_cast<void>(
            stockagent::run_walk_forward(
                ticks, candidates));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(name, threw);
}

void test_market_data_fingerprint_known_vector() {
    constexpr std::string_view name =
        "market_data_fingerprint_known_vector";
    const std::vector<stockagent::MarketTick> ticks{
        stockagent::MarketTick{1U, 2U, 3, 4, 5, 6},
    };
    CHECK(name,
          stockagent::fingerprint_market_data(ticks) ==
              11'811'066'898'527'378'914ULL);
    CHECK(name,
          stockagent::fingerprint_market_data({}) ==
              14'695'981'039'346'656'037ULL);
}

void test_deterministic_research_replay() {
    constexpr std::string_view name =
        "deterministic_research_replay";
    const auto ticks = make_trending_ticks(1'000U);
    auto config =
        stockagent::default_backtest_candidates().front();
    const auto first =
        stockagent::run_backtest(
            ticks, 100U, ticks.size(), config);
    const auto second =
        stockagent::run_backtest(
            ticks, 100U, ticks.size(), config);
    CHECK(name,
          first.metrics.net_liquidation_pnl_ticks ==
              second.metrics.net_liquidation_pnl_ticks);
    CHECK(name,
          first.metrics.max_drawdown_ticks ==
              second.metrics.max_drawdown_ticks);
    CHECK(name,
          first.metrics.fills == second.metrics.fills);
    CHECK(name, first.trades.size() == second.trades.size());
}

void test_invalid_inputs_are_rejected() {
    constexpr std::string_view name =
        "invalid_inputs_are_rejected";
    auto ticks = make_trending_ticks(20U);
    ticks[5].bid_ticks = ticks[5].ask_ticks + 1;
    bool threw = false;
    try {
        static_cast<void>(
            stockagent::run_backtest(
                ticks, 0U, ticks.size(),
                buy_hold_config()));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(name, threw);

    auto invalid = buy_hold_config();
    invalid.execution.max_participation_bps = 0U;
    threw = false;
    try {
        const auto valid_ticks = make_trending_ticks(20U);
        static_cast<void>(
            stockagent::run_backtest(
                valid_ticks, 0U,
                valid_ticks.size(), invalid));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(name, threw);

    invalid = buy_hold_config();
    invalid.signal.entry_threshold = 1.0;
    threw = false;
    try {
        const auto valid_ticks = make_trending_ticks(20U);
        static_cast<void>(
            stockagent::run_backtest(
                valid_ticks, 0U,
                valid_ticks.size(), invalid));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(name, threw);
}

}  // namespace

int main() {
    try {
        test_public_signal_families();
        test_causal_fill_and_latency();
        test_future_quotes_change_fill_not_decision();
        test_cost_monotonicity_and_decomposition();
        test_participation_cap_and_position_limit();
        test_multi_slice_forced_liquidation();
        test_short_side_cost_identity();
        test_participation_floor_can_produce_no_fill();
        test_incomplete_causal_liquidation_is_ineligible();
        test_liquidation_tail_cancels_risk_seeking_pending_intent();
        test_unavailable_path_liquidation_mark_is_ineligible();
        test_score_is_scale_normalized_and_requires_closed_trades();
        test_distribution_metric_sample_floors();
        test_walk_forward_final_test_isolation();
        test_walk_forward_reports_no_trade_gate();
        test_scale_tie_uses_stable_catalog_order();
        test_one_nonpositive_fold_recommends_no_trade();
        test_walk_forward_requires_common_evaluation_regime();
        test_market_data_fingerprint_known_vector();
        test_deterministic_research_replay();
        test_invalid_inputs_are_rejected();
    } catch (const std::exception& error) {
        std::cerr << "UNEXPECTED EXCEPTION: "
                  << error.what() << '\n';
        return 2;
    }

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout
        << "All StockAgent backtest tests passed\n";
    return 0;
}
