#include "stockagent/backtest.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace stockagent {
namespace {

constexpr std::size_t maximum_indicator_window = 1'000'000;
constexpr Quantity maximum_research_quantity = 1'000'000'000;

[[nodiscard]] std::uint64_t quantity_magnitude(
    const Quantity value) noexcept {
    return value < 0
               ? static_cast<std::uint64_t>(-(value + 1)) + 1U
               : static_cast<std::uint64_t>(value);
}

[[nodiscard]] Price mid_price(const MarketTick& tick) noexcept {
    return tick.bid_ticks +
           ((tick.ask_ticks - tick.bid_ticks) / 2);
}

void validate_signal_config(const ResearchSignalConfig& config) {
    if (config.fast_window == 0U ||
        config.slow_window <= config.fast_window ||
        config.mean_reversion_window < 2U ||
        config.volatility_window < 2U ||
        config.imbalance_window == 0U ||
        config.fast_window > maximum_indicator_window ||
        config.slow_window > maximum_indicator_window ||
        config.mean_reversion_window > maximum_indicator_window ||
        config.volatility_window > maximum_indicator_window ||
        config.imbalance_window > maximum_indicator_window) {
        throw std::invalid_argument(
            "indicator windows are invalid or too large");
    }

    const double weight_sum =
        config.trend_weight + config.mean_reversion_weight +
        config.imbalance_weight;
    if (!std::isfinite(config.trend_weight) ||
        !std::isfinite(config.mean_reversion_weight) ||
        !std::isfinite(config.imbalance_weight) ||
        config.trend_weight < 0.0 ||
        config.mean_reversion_weight < 0.0 ||
        config.imbalance_weight < 0.0 ||
        !std::isfinite(weight_sum) || weight_sum <= 0.0 ||
        !std::isfinite(config.volatility_target_ticks) ||
        config.volatility_target_ticks <= 0.0 ||
        !std::isfinite(config.entry_threshold) ||
        !std::isfinite(config.exit_threshold) ||
        config.entry_threshold <= 0.0 ||
        config.entry_threshold >= 1.0 ||
        config.exit_threshold < 0.0 ||
        config.exit_threshold >= config.entry_threshold ||
        !std::isfinite(config.mean_reversion_trend_filter) ||
        config.mean_reversion_trend_filter < 0.0 ||
        config.mean_reversion_trend_filter > 1.0) {
        throw std::invalid_argument(
            "signal weights, volatility target, or thresholds are invalid");
    }
    if (config.base_target_quantity <= 0 ||
        config.max_absolute_position <= 0 ||
        config.max_order_quantity <= 0 ||
        config.base_target_quantity > config.max_absolute_position ||
        config.max_order_quantity > config.max_absolute_position ||
        config.max_absolute_position > maximum_research_quantity ||
        config.rebalance_interval_events == 0U ||
        config.max_holding_events == 0U) {
        throw std::invalid_argument(
            "position, order, rebalance, or holding limits are invalid");
    }
}

void validate_execution_config(const ExecutionCostConfig& config) {
    if (config.fixed_fee_ticks < 0 ||
        config.fee_ticks_per_unit < 0 ||
        config.base_slippage_ticks < 0 ||
        !std::isfinite(config.impact_coefficient_ticks) ||
        config.impact_coefficient_ticks < 0.0 ||
        config.impact_coefficient_ticks > 1'000'000.0 ||
        config.max_participation_bps == 0U ||
        config.max_participation_bps > 10'000U) {
        throw std::invalid_argument(
            "execution cost or participation assumptions are invalid");
    }
}

[[nodiscard]] bool same_evaluation_regime(
    const BacktestConfig& left,
    const BacktestConfig& right) noexcept {
    return left.execution.fixed_fee_ticks ==
               right.execution.fixed_fee_ticks &&
           left.execution.fee_ticks_per_unit ==
               right.execution.fee_ticks_per_unit &&
           left.execution.base_slippage_ticks ==
               right.execution.base_slippage_ticks &&
           left.execution.impact_coefficient_ticks ==
               right.execution.impact_coefficient_ticks &&
           left.execution.max_participation_bps ==
               right.execution.max_participation_bps &&
           left.execution.latency_ns ==
               right.execution.latency_ns &&
           left.warmup_events == right.warmup_events &&
           left.metric_bucket_events ==
               right.metric_bucket_events &&
           left.liquidation_tail_events ==
               right.liquidation_tail_events;
}

void validate_market_range(const std::vector<MarketTick>& ticks,
                           const std::size_t begin,
                           const std::size_t end) {
    if (begin >= end || end > ticks.size()) {
        throw std::invalid_argument("backtest range is empty or invalid");
    }
    Sequence previous_sequence{};
    TimestampNs previous_timestamp{};
    bool first = true;
    for (std::size_t index = begin; index < end; ++index) {
        const auto& tick = ticks[index];
        if (tick.bid_ticks <= 0 ||
            tick.ask_ticks < tick.bid_ticks ||
            tick.bid_quantity <= 0 ||
            tick.ask_quantity <= 0) {
            throw std::invalid_argument(
                "backtest input contains an invalid quote");
        }
        if (!first &&
            (tick.sequence <= previous_sequence ||
             tick.timestamp_ns <= previous_timestamp)) {
            throw std::invalid_argument(
                "backtest input must have strict sequence and time order");
        }
        first = false;
        previous_sequence = tick.sequence;
        previous_timestamp = tick.timestamp_ns;
    }
}

[[nodiscard]] std::uint64_t participation_capacity(
    const Quantity displayed_quantity,
    const std::uint32_t participation_bps) noexcept {
    const auto displayed =
        static_cast<std::uint64_t>(displayed_quantity);
    constexpr std::uint64_t basis_points = 10'000U;
    const auto whole =
        (displayed / basis_points) *
        static_cast<std::uint64_t>(participation_bps);
    const auto remainder =
        ((displayed % basis_points) *
         static_cast<std::uint64_t>(participation_bps)) /
        basis_points;
    return whole + remainder;
}

[[nodiscard]] Price modeled_slippage_ticks(
    const ExecutionCostConfig& config, const Quantity quantity,
    const Quantity displayed_quantity) {
    const long double participation =
        static_cast<long double>(quantity) /
        static_cast<long double>(displayed_quantity);
    const long double impact =
        static_cast<long double>(config.impact_coefficient_ticks) *
        std::sqrt(std::clamp(participation, 0.0L, 1.0L));
    const long double total =
        static_cast<long double>(config.base_slippage_ticks) +
        std::ceil(impact);
    if (!std::isfinite(total) ||
        total >
            static_cast<long double>(
                std::numeric_limits<Price>::max())) {
        throw std::overflow_error("modeled slippage is not representable");
    }
    return static_cast<Price>(total);
}

[[nodiscard]] Price fill_price_for(
    const Side side, const Price reference_bbo,
    const Price slippage_ticks) {
    if (side == Side::Buy) {
        if (reference_bbo >
            std::numeric_limits<Price>::max() - slippage_ticks) {
            throw std::overflow_error("buy fill price overflow");
        }
        return reference_bbo + slippage_ticks;
    }
    if (reference_bbo <= slippage_ticks) {
        throw std::overflow_error("sell fill price is not positive");
    }
    return reference_bbo - slippage_ticks;
}

[[nodiscard]] long double execution_fee(
    const ExecutionCostConfig& config,
    const Quantity quantity) noexcept {
    return static_cast<long double>(config.fixed_fee_ticks) +
           (static_cast<long double>(config.fee_ticks_per_unit) *
            static_cast<long double>(quantity));
}

struct PendingIntent {
    Quantity target_position{};
    Sequence decision_sequence{};
    TimestampNs decision_timestamp_ns{};
    Price decision_mid_ticks{};
};

struct PortfolioState {
    Quantity position{};
    long double cash_ticks{};
    long double gross_cash_ticks{};
    long double episode_start_equity_ticks{};
    bool episode_open{false};
};

struct LiquidationMark {
    long double gross_ticks{};
    long double net_ticks{};
    long double terminal_fee_ticks{};
    long double terminal_spread_ticks{};
    long double terminal_slippage_ticks{};
    bool modeled_liquidation_complete{true};
};

[[nodiscard]] LiquidationMark mark_to_liquidation(
    const PortfolioState& state, const MarketTick& tick,
    const ExecutionCostConfig& execution,
    const Quantity max_order_quantity) {
    const Price mid = mid_price(tick);
    LiquidationMark mark;
    mark.gross_ticks =
        state.gross_cash_ticks +
        (static_cast<long double>(state.position) *
         static_cast<long double>(mid));
    if (state.position == 0) {
        mark.net_ticks = state.cash_ticks;
        return mark;
    }

    const Side closing_side =
        state.position > 0 ? Side::Sell : Side::Buy;
    const auto magnitude = quantity_magnitude(state.position);
    if (magnitude >
        static_cast<std::uint64_t>(
            std::numeric_limits<Quantity>::max())) {
        throw std::overflow_error(
            "terminal position magnitude is not representable");
    }
    const Quantity displayed =
        closing_side == Side::Buy ? tick.ask_quantity
                                  : tick.bid_quantity;
    const Price reference =
        closing_side == Side::Buy ? tick.ask_ticks
                                  : tick.bid_ticks;
    const auto capacity =
        participation_capacity(
            displayed, execution.max_participation_bps);
    const auto slice_limit =
        std::min(
            capacity,
            static_cast<std::uint64_t>(max_order_quantity));
    mark.net_ticks = state.cash_ticks;
    if (slice_limit == 0U) {
        mark.net_ticks +=
            static_cast<long double>(state.position) *
            static_cast<long double>(reference);
        const long double side_sign =
            closing_side == Side::Buy ? 1.0L : -1.0L;
        mark.terminal_spread_ticks =
            side_sign *
            static_cast<long double>(reference - mid) *
            static_cast<long double>(magnitude);
        mark.modeled_liquidation_complete = false;
        return mark;
    }

    const auto apply_slices =
        [&](const std::uint64_t slice_quantity,
            const std::uint64_t slice_count) {
            if (slice_quantity == 0U || slice_count == 0U) {
                return;
            }
            const auto quantity_per_slice =
                static_cast<Quantity>(slice_quantity);
            const Price slippage =
                modeled_slippage_ticks(
                    execution, quantity_per_slice,
                    displayed);
            const Price liquidation_price =
                fill_price_for(
                    closing_side, reference, slippage);
            const long double total_quantity =
                static_cast<long double>(slice_quantity) *
                static_cast<long double>(slice_count);
            const long double fee =
                (static_cast<long double>(
                     execution.fixed_fee_ticks) *
                 static_cast<long double>(slice_count)) +
                (static_cast<long double>(
                     execution.fee_ticks_per_unit) *
                 total_quantity);
            const long double signed_value =
                static_cast<long double>(liquidation_price) *
                total_quantity;
            mark.net_ticks +=
                (closing_side == Side::Sell
                     ? signed_value
                     : -signed_value) -
                fee;

            const long double side_sign =
                closing_side == Side::Buy ? 1.0L : -1.0L;
            mark.terminal_fee_ticks += fee;
            mark.terminal_spread_ticks +=
                side_sign *
                static_cast<long double>(reference - mid) *
                total_quantity;
            mark.terminal_slippage_ticks +=
                side_sign *
                static_cast<long double>(
                    liquidation_price - reference) *
                total_quantity;
        };

    const std::uint64_t full_slices =
        magnitude / slice_limit;
    const std::uint64_t remainder =
        magnitude % slice_limit;
    apply_slices(slice_limit, full_slices);
    apply_slices(remainder, 1U);
    return mark;
}

void finalize_distribution_metrics(
    BacktestMetrics& metrics,
    const std::vector<long double>& buckets) {
    metrics.metric_buckets =
        static_cast<std::uint64_t>(buckets.size());
    if (buckets.empty()) {
        return;
    }

    long double sum{};
    long double downside_square_sum{};
    for (const auto value : buckets) {
        sum += value;
        if (value < 0.0L) {
            downside_square_sum += value * value;
        }
    }
    const long double count =
        static_cast<long double>(buckets.size());
    metrics.mean_bucket_pnl_ticks = sum / count;
    metrics.downside_deviation_ticks =
        std::sqrt(downside_square_sum / count);
    metrics.worst_bucket_pnl_ticks =
        *std::min_element(buckets.begin(), buckets.end());

    if (buckets.size() >= 2U) {
        long double square_sum{};
        for (const auto value : buckets) {
            const long double deviation =
                value - metrics.mean_bucket_pnl_ticks;
            square_sum += deviation * deviation;
        }
        metrics.bucket_volatility_ticks =
            std::sqrt(square_sum /
                      static_cast<long double>(buckets.size() - 1U));
        constexpr long double epsilon = 1.0e-18L;
        if (buckets.size() >= 20U &&
            metrics.bucket_volatility_ticks > epsilon) {
            metrics.sharpe_per_sqrt_bucket =
                metrics.mean_bucket_pnl_ticks /
                metrics.bucket_volatility_ticks;
            metrics.sharpe_available = true;
        }
        if (buckets.size() >= 20U &&
            metrics.downside_deviation_ticks > epsilon) {
            metrics.sortino_per_sqrt_bucket =
                metrics.mean_bucket_pnl_ticks /
                metrics.downside_deviation_ticks;
            metrics.sortino_available = true;
        }
    }

    if (buckets.size() >= 40U) {
        auto ordered = buckets;
        std::sort(ordered.begin(), ordered.end());
        const std::size_t tail_count =
            std::max<std::size_t>(
                1U,
                (ordered.size() + 19U) / 20U);
        long double tail_sum{};
        for (std::size_t index = 0; index < tail_count; ++index) {
            tail_sum += ordered[index];
        }
        metrics.mean_worst_5pct_bucket_pnl_ticks =
            tail_sum / static_cast<long double>(tail_count);
        metrics.tail_metric_available = true;
    }
}

void finalize_closed_trade_metrics(
    BacktestMetrics& metrics,
    const std::vector<long double>& trades) {
    metrics.closed_trades =
        static_cast<std::uint64_t>(trades.size());
    if (trades.empty()) {
        return;
    }

    long double profit_sum{};
    long double loss_sum{};
    long double total{};
    for (const auto value : trades) {
        total += value;
        if (value > 0.0L) {
            profit_sum += value;
            ++metrics.winning_closed_trades;
        } else if (value < 0.0L) {
            loss_sum -= value;
        }
    }
    metrics.average_closed_trade_pnl_ticks =
        total / static_cast<long double>(trades.size());
    metrics.closed_trade_hit_rate =
        static_cast<long double>(metrics.winning_closed_trades) /
        static_cast<long double>(trades.size());
    if (loss_sum > 0.0L) {
        metrics.closed_trade_profit_factor =
            profit_sum / loss_sum;
    } else if (profit_sum > 0.0L) {
        metrics.closed_trade_profit_factor =
            std::numeric_limits<long double>::infinity();
    }
}

[[nodiscard]] BacktestResult aggregate_results(
    const std::vector<BacktestResult>& results) {
    BacktestResult aggregate;
    long double exposure_weighted_sum{};
    long double time_in_market_weighted_sum{};
    long double stitched_equity{};
    long double stitched_peak{};
    std::uint64_t drawdown_duration{};
    bool all_terminal_liquidations_complete = true;
    bool all_liquidation_marks_available = true;

    for (const auto& result : results) {
        const auto& source = result.metrics;
        auto& target = aggregate.metrics;
        target.gross_liquidation_pnl_ticks +=
            source.gross_liquidation_pnl_ticks;
        target.net_liquidation_pnl_ticks +=
            source.net_liquidation_pnl_ticks;
        target.executed_fill_shortfall_ticks +=
            source.executed_fill_shortfall_ticks;
        target.modeled_total_shortfall_ticks +=
            source.modeled_total_shortfall_ticks;
        target.forced_liquidation_fill_shortfall_ticks +=
            source.forced_liquidation_fill_shortfall_ticks;
        target.residual_modeled_liquidation_cost_ticks +=
            source.residual_modeled_liquidation_cost_ticks;
        target.fees_ticks += source.fees_ticks;
        target.spread_cost_ticks += source.spread_cost_ticks;
        target.slippage_cost_ticks += source.slippage_cost_ticks;
        target.latency_cost_ticks += source.latency_cost_ticks;
        target.quantity_turnover += source.quantity_turnover;
        target.events += source.events;
        target.intents_submitted += source.intents_submitted;
        target.intents_replaced += source.intents_replaced;
        target.fills += source.fills;
        target.partial_or_capacity_limited_fills +=
            source.partial_or_capacity_limited_fills;
        target.cancelled_unfilled_intents +=
            source.cancelled_unfilled_intents;
        target.maximum_absolute_position =
            std::max(target.maximum_absolute_position,
                     source.maximum_absolute_position);
        exposure_weighted_sum +=
            source.average_absolute_exposure *
            static_cast<long double>(source.events);
        time_in_market_weighted_sum +=
            source.time_in_market_ratio *
            static_cast<long double>(source.events);
        all_terminal_liquidations_complete =
            all_terminal_liquidations_complete &&
            source.terminal_liquidation_complete;
        all_liquidation_marks_available =
            all_liquidation_marks_available &&
            source.all_liquidation_marks_available;

        aggregate.bucket_pnl_ticks.insert(
            aggregate.bucket_pnl_ticks.end(),
            result.bucket_pnl_ticks.begin(),
            result.bucket_pnl_ticks.end());
        aggregate.closed_trade_pnl_ticks.insert(
            aggregate.closed_trade_pnl_ticks.end(),
            result.closed_trade_pnl_ticks.begin(),
            result.closed_trade_pnl_ticks.end());

        long double previous_fold_equity{};
        for (const auto& point : result.equity_curve) {
            stitched_equity +=
                point.net_liquidation_pnl_ticks -
                previous_fold_equity;
            previous_fold_equity =
                point.net_liquidation_pnl_ticks;
            if (stitched_equity >= stitched_peak) {
                stitched_peak = stitched_equity;
                drawdown_duration = 0U;
            } else {
                ++drawdown_duration;
                const long double drawdown =
                    stitched_peak - stitched_equity;
                if (drawdown > target.max_drawdown_ticks) {
                    target.max_drawdown_ticks = drawdown;
                }
                target.max_drawdown_duration_events =
                    std::max(
                        target.max_drawdown_duration_events,
                        drawdown_duration);
            }
        }
    }

    if (aggregate.metrics.events > 0U) {
        const long double events =
            static_cast<long double>(aggregate.metrics.events);
        aggregate.metrics.average_absolute_exposure =
            exposure_weighted_sum / events;
        aggregate.metrics.time_in_market_ratio =
            time_in_market_weighted_sum / events;
    }
    aggregate.metrics.terminal_liquidation_complete =
        all_terminal_liquidations_complete;
    aggregate.metrics.all_liquidation_marks_available =
        all_liquidation_marks_available;
    finalize_distribution_metrics(
        aggregate.metrics, aggregate.bucket_pnl_ticks);
    finalize_closed_trade_metrics(
        aggregate.metrics, aggregate.closed_trade_pnl_ticks);
    return aggregate;
}

[[nodiscard]] long double median(
    std::vector<long double> values) {
    if (values.empty()) {
        return -std::numeric_limits<long double>::infinity();
    }
    std::sort(values.begin(), values.end());
    const std::size_t middle = values.size() / 2U;
    if ((values.size() % 2U) != 0U) {
        return values[middle];
    }
    return (values[middle - 1U] + values[middle]) / 2.0L;
}

[[nodiscard]] ExecutionCostConfig scenario_execution(
    const ExecutionCostConfig& base) {
    ExecutionCostConfig scenario = base;
    const auto double_bounded = [](const std::int64_t value) {
        if (value > std::numeric_limits<std::int64_t>::max() / 2) {
            return std::numeric_limits<std::int64_t>::max();
        }
        return value * 2;
    };
    scenario.fixed_fee_ticks =
        double_bounded(base.fixed_fee_ticks);
    scenario.fee_ticks_per_unit =
        double_bounded(base.fee_ticks_per_unit);
    scenario.base_slippage_ticks =
        base.base_slippage_ticks >
                std::numeric_limits<Price>::max() / 2
            ? std::numeric_limits<Price>::max()
            : base.base_slippage_ticks * 2;
    scenario.impact_coefficient_ticks =
        std::min(1'000'000.0,
                 base.impact_coefficient_ticks * 2.0);
    scenario.latency_ns =
        base.latency_ns >
                std::numeric_limits<TimestampNs>::max() / 2U
            ? std::numeric_limits<TimestampNs>::max()
            : base.latency_ns * 2U;
    return scenario;
}

}  // namespace

ResearchSignalModel::ResearchSignalModel(ResearchSignalConfig config)
    : config_(config) {
    validate_signal_config(config_);
    mean_prices_.assign(
        config_.mean_reversion_window, 0.0);
}

std::size_t ResearchSignalModel::required_warmup_events() const noexcept {
    switch (config_.strategy) {
        case ResearchStrategy::Trend:
            return std::max(config_.slow_window,
                            config_.volatility_window);
        case ResearchStrategy::MeanReversion:
            return std::max(
                {config_.slow_window,
                 config_.mean_reversion_window,
                 config_.volatility_window});
        case ResearchStrategy::OrderBookImbalance:
            return std::max(config_.imbalance_window,
                            config_.volatility_window);
        case ResearchStrategy::Ensemble:
            return std::max(
                {config_.slow_window,
                 config_.mean_reversion_window,
                 config_.volatility_window,
                 config_.imbalance_window});
        case ResearchStrategy::BuyAndHold:
        case ResearchStrategy::NoTrade:
            return 1U;
    }
    return 1U;
}

std::optional<Quantity> ResearchSignalModel::on_tick(
    const MarketTick& tick, const Quantity current_position) {
    ++event_count_;
    const double mid = static_cast<double>(mid_price(tick));
    const double fast_alpha =
        2.0 / (static_cast<double>(config_.fast_window) + 1.0);
    const double slow_alpha =
        2.0 / (static_cast<double>(config_.slow_window) + 1.0);
    const double volatility_alpha =
        2.0 /
        (static_cast<double>(config_.volatility_window) + 1.0);
    const double imbalance_alpha =
        2.0 /
        (static_cast<double>(config_.imbalance_window) + 1.0);

    if (!initialized_) {
        initialized_ = true;
        fast_ema_ = mid;
        slow_ema_ = mid;
        previous_mid_ = mid;
    } else {
        const double change = mid - previous_mid_;
        fast_ema_ += fast_alpha * (mid - fast_ema_);
        slow_ema_ += slow_alpha * (mid - slow_ema_);
        return_variance_ewma_ =
            ((1.0 - volatility_alpha) *
             return_variance_ewma_) +
            (volatility_alpha * change * change);
        previous_mid_ = mid;
    }
    last_volatility_ticks_ =
        std::max(0.25, std::sqrt(return_variance_ewma_));

    const long double bid_quantity =
        static_cast<long double>(tick.bid_quantity);
    const long double ask_quantity =
        static_cast<long double>(tick.ask_quantity);
    const long double total_quantity =
        bid_quantity + ask_quantity;
    const long double book_imbalance =
        (bid_quantity - ask_quantity) / total_quantity;
    const long double microprice =
        ((static_cast<long double>(tick.ask_ticks) *
          bid_quantity) +
         (static_cast<long double>(tick.bid_ticks) *
          ask_quantity)) /
        total_quantity;
    const long double half_spread =
        std::max(
            0.5L,
            static_cast<long double>(
                tick.ask_ticks - tick.bid_ticks) /
                2.0L);
    const long double microprice_signal =
        std::clamp(
            (microprice - static_cast<long double>(mid)) /
                half_spread,
            -1.0L, 1.0L);
    const double raw_imbalance =
        static_cast<double>(
            std::clamp(
                (book_imbalance + microprice_signal) / 2.0L,
                -1.0L, 1.0L));
    if (event_count_ == 1U) {
        imbalance_ewma_ = raw_imbalance;
    } else {
        imbalance_ewma_ +=
            imbalance_alpha *
            (raw_imbalance - imbalance_ewma_);
    }

    if (mean_count_ == mean_prices_.size()) {
        const double outgoing = mean_prices_[mean_cursor_];
        mean_sum_ -= static_cast<long double>(outgoing);
        mean_square_sum_ -=
            static_cast<long double>(outgoing) *
            static_cast<long double>(outgoing);
    } else {
        ++mean_count_;
    }
    mean_prices_[mean_cursor_] = mid;
    mean_cursor_ = (mean_cursor_ + 1U) % mean_prices_.size();
    mean_sum_ += static_cast<long double>(mid);
    mean_square_sum_ +=
        static_cast<long double>(mid) *
        static_cast<long double>(mid);

    const long double observation_count =
        static_cast<long double>(mean_count_);
    const long double rolling_mean =
        mean_sum_ / observation_count;
    const long double rolling_variance =
        std::max(
            0.0L,
            (mean_square_sum_ / observation_count) -
                (rolling_mean * rolling_mean));
    const long double rolling_stddev =
        std::sqrt(rolling_variance);
    const double mean_reversion_signal =
        rolling_stddev > 1.0e-12L
            ? -std::tanh(
                  static_cast<double>(
                      (static_cast<long double>(mid) -
                       rolling_mean) /
                      (2.0L * rolling_stddev)))
            : 0.0;
    const double trend_denominator =
        last_volatility_ticks_ *
        std::max(
            2.0,
            std::sqrt(
                static_cast<double>(config_.slow_window)));
    const double trend_signal =
        std::tanh((fast_ema_ - slow_ema_) /
                  trend_denominator);
    const double imbalance_signal =
        std::clamp(imbalance_ewma_, -1.0, 1.0);

    switch (config_.strategy) {
        case ResearchStrategy::Trend:
            last_score_ = trend_signal;
            break;
        case ResearchStrategy::MeanReversion:
            last_score_ = mean_reversion_signal;
            break;
        case ResearchStrategy::OrderBookImbalance:
            last_score_ = imbalance_signal;
            break;
        case ResearchStrategy::Ensemble: {
            const double weight_sum =
                config_.trend_weight +
                config_.mean_reversion_weight +
                config_.imbalance_weight;
            last_score_ =
                ((config_.trend_weight * trend_signal) +
                 (config_.mean_reversion_weight *
                  mean_reversion_signal) +
                 (config_.imbalance_weight *
                  imbalance_signal)) /
                weight_sum;
            break;
        }
        case ResearchStrategy::BuyAndHold:
            last_score_ = 1.0;
            break;
        case ResearchStrategy::NoTrade:
            last_score_ = 0.0;
            break;
    }
    last_score_ = std::clamp(last_score_, -1.0, 1.0);

    if (current_position == 0) {
        holding_events_ = 0U;
    } else {
        ++holding_events_;
    }
    if (event_count_ < required_warmup_events() ||
        (event_count_ % config_.rebalance_interval_events) != 0U) {
        return std::nullopt;
    }

    if (config_.strategy == ResearchStrategy::NoTrade) {
        return std::nullopt;
    }
    if (config_.strategy == ResearchStrategy::BuyAndHold) {
        if (current_position == config_.base_target_quantity) {
            return std::nullopt;
        }
        return config_.base_target_quantity;
    }
    if (config_.strategy == ResearchStrategy::MeanReversion &&
        std::abs(trend_signal) >
            config_.mean_reversion_trend_filter) {
        return current_position == 0
                   ? std::nullopt
                   : std::optional<Quantity>{0};
    }
    if (current_position != 0 &&
        holding_events_ >= config_.max_holding_events) {
        return Quantity{0};
    }

    const double absolute_score = std::abs(last_score_);
    if (absolute_score <= config_.exit_threshold) {
        return current_position == 0
                   ? std::nullopt
                   : std::optional<Quantity>{0};
    }
    if (absolute_score < config_.entry_threshold) {
        return std::nullopt;
    }

    const double volatility_scale =
        std::clamp(
            config_.volatility_target_ticks /
                last_volatility_ticks_,
            0.25, 2.0);
    const double conviction =
        std::clamp(
            (absolute_score - config_.entry_threshold) /
                (1.0 - config_.entry_threshold),
            0.0, 1.0);
    const double raw_quantity =
        static_cast<double>(config_.base_target_quantity) *
        volatility_scale *
        (0.25 + (0.75 * conviction));
    const auto rounded =
        static_cast<Quantity>(std::llround(raw_quantity));
    const Quantity magnitude =
        std::clamp(
            rounded, Quantity{1},
            config_.max_absolute_position);
    const Quantity target =
        last_score_ > 0.0 ? magnitude : -magnitude;

    if ((current_position > 0 && target < 0) ||
        (current_position < 0 && target > 0)) {
        return Quantity{0};
    }
    if (target == current_position) {
        return std::nullopt;
    }
    return target;
}

BacktestResult run_backtest(
    const std::vector<MarketTick>& ticks, const std::size_t begin,
    const std::size_t end, const BacktestConfig& config) {
    validate_signal_config(config.signal);
    validate_execution_config(config.execution);
    if (begin >= end || end > ticks.size()) {
        throw std::invalid_argument(
            "backtest range is empty or invalid");
    }
    if (config.metric_bucket_events == 0U) {
        throw std::invalid_argument(
            "metric_bucket_events must be positive");
    }
    const std::size_t range_events = end - begin;
    if (config.liquidation_tail_events == 0U ||
        range_events <= 2U ||
        config.liquidation_tail_events >=
            (range_events - 2U)) {
        throw std::invalid_argument(
            "backtest range must leave strategy events before "
            "the liquidation tail");
    }
    const std::size_t liquidation_begin =
        end - config.liquidation_tail_events;

    ResearchSignalModel model(config.signal);
    const std::size_t warmup_events =
        std::max(config.warmup_events,
                 model.required_warmup_events());
    const std::size_t warmup_begin =
        begin > warmup_events ? begin - warmup_events : 0U;
    validate_market_range(ticks, warmup_begin, end);

    for (std::size_t index = warmup_begin; index < begin; ++index) {
        static_cast<void>(model.on_tick(ticks[index], 0));
    }

    BacktestResult result;
    result.trades.reserve((end - begin) / 8U);
    result.equity_curve.reserve(end - begin);
    PortfolioState portfolio;
    std::optional<PendingIntent> pending;
    long double actual_fees{};
    long double actual_spread_cost{};
    long double actual_slippage_cost{};
    long double actual_latency_cost{};
    long double forced_liquidation_fill_shortfall{};
    long double peak_equity{};
    long double previous_bucket_equity{};
    long double exposure_sum{};
    std::uint64_t events_in_market{};
    std::uint64_t current_drawdown_duration{};
    bool all_liquidation_marks_available = true;

    for (std::size_t index = begin; index < end; ++index) {
        const auto& tick = ticks[index];
        const Price current_mid = mid_price(tick);
        const bool liquidation_event =
            index >= liquidation_begin;

        if (liquidation_event &&
            pending.has_value() &&
            pending->target_position != 0) {
            ++result.metrics.intents_replaced;
            pending.reset();
        }

        if (pending.has_value() &&
            tick.sequence > pending->decision_sequence &&
            tick.timestamp_ns >= pending->decision_timestamp_ns &&
            (tick.timestamp_ns -
             pending->decision_timestamp_ns) >=
                config.execution.latency_ns) {
            Quantity effective_target = pending->target_position;
            if ((portfolio.position > 0 && effective_target < 0) ||
                (portfolio.position < 0 && effective_target > 0)) {
                effective_target = 0;
            }
            const Quantity delta =
                effective_target - portfolio.position;
            if (delta != 0) {
                const Side side =
                    delta > 0 ? Side::Buy : Side::Sell;
                const auto desired_magnitude =
                    quantity_magnitude(delta);
                const Quantity displayed =
                    side == Side::Buy ? tick.ask_quantity
                                      : tick.bid_quantity;
                const auto capacity =
                    participation_capacity(
                        displayed,
                        config.execution.max_participation_bps);
                const auto order_limit =
                    static_cast<std::uint64_t>(
                        config.signal.max_order_quantity);
                const auto fill_magnitude =
                    std::min(
                        {desired_magnitude, capacity,
                         order_limit});
                if (fill_magnitude > 0U) {
                    const auto quantity =
                        static_cast<Quantity>(fill_magnitude);
                    const Price reference =
                        side == Side::Buy ? tick.ask_ticks
                                          : tick.bid_ticks;
                    const Price slippage =
                        modeled_slippage_ticks(
                            config.execution, quantity,
                            displayed);
                    const Price fill_price =
                        fill_price_for(
                            side, reference, slippage);
                    const long double fee =
                        execution_fee(config.execution,
                                      quantity);
                    const long double side_sign =
                        side == Side::Buy ? 1.0L : -1.0L;
                    const long double spread_cost =
                        side_sign *
                        static_cast<long double>(
                            reference - current_mid) *
                        static_cast<long double>(quantity);
                    const long double slippage_cost =
                        side_sign *
                        static_cast<long double>(
                            fill_price - reference) *
                        static_cast<long double>(quantity);
                    const long double latency_cost =
                        side_sign *
                        static_cast<long double>(
                            current_mid -
                            pending->decision_mid_ticks) *
                        static_cast<long double>(quantity);

                    const auto before =
                        mark_to_liquidation(
                            portfolio, tick,
                            config.execution,
                            config.signal.max_order_quantity);
                    const Quantity old_position =
                        portfolio.position;
                    const long double actual_notional =
                        static_cast<long double>(fill_price) *
                        static_cast<long double>(quantity);
                    const long double gross_notional =
                        static_cast<long double>(
                            pending->decision_mid_ticks) *
                        static_cast<long double>(quantity);
                    if (side == Side::Buy) {
                        portfolio.cash_ticks -=
                            actual_notional + fee;
                        portfolio.gross_cash_ticks -=
                            gross_notional;
                        portfolio.position += quantity;
                    } else {
                        portfolio.cash_ticks +=
                            actual_notional - fee;
                        portfolio.gross_cash_ticks +=
                            gross_notional;
                        portfolio.position -= quantity;
                    }

                    if (old_position == 0 &&
                        portfolio.position != 0) {
                        portfolio.episode_start_equity_ticks =
                            before.net_ticks;
                        portfolio.episode_open = true;
                    } else if (old_position != 0 &&
                               portfolio.position == 0 &&
                               portfolio.episode_open) {
                        result.closed_trade_pnl_ticks.push_back(
                            portfolio.cash_ticks -
                            portfolio
                                .episode_start_equity_ticks);
                        portfolio.episode_open = false;
                    }

                    actual_fees += fee;
                    actual_spread_cost += spread_cost;
                    actual_slippage_cost += slippage_cost;
                    actual_latency_cost += latency_cost;
                    if (liquidation_event) {
                        forced_liquidation_fill_shortfall +=
                            fee + spread_cost +
                            slippage_cost + latency_cost;
                    }
                    result.metrics.quantity_turnover +=
                        static_cast<long double>(quantity);
                    ++result.metrics.fills;
                    if (fill_magnitude < desired_magnitude) {
                        ++result.metrics
                              .partial_or_capacity_limited_fills;
                        ++result.metrics
                              .cancelled_unfilled_intents;
                    }
                    result.trades.push_back(
                        TradeRecord{
                            pending->decision_sequence,
                            tick.sequence,
                            pending->decision_timestamp_ns,
                            tick.timestamp_ns,
                            side,
                            quantity,
                            pending->decision_mid_ticks,
                            reference,
                            fill_price,
                            fee,
                            spread_cost,
                            slippage_cost,
                            latency_cost,
                            liquidation_event,
                        });
                } else {
                    ++result.metrics
                          .cancelled_unfilled_intents;
                }
            }
            pending.reset();
        }

        if (liquidation_event) {
            if (portfolio.position != 0 &&
                !pending.has_value()) {
                pending = PendingIntent{
                    0,
                    tick.sequence,
                    tick.timestamp_ns,
                    current_mid,
                };
                ++result.metrics.intents_submitted;
            } else if (portfolio.position == 0 &&
                       pending.has_value()) {
                ++result.metrics.intents_replaced;
                pending.reset();
            }
        } else {
            const auto target =
                model.on_tick(tick, portfolio.position);
            if (target.has_value() &&
                *target == portfolio.position) {
                if (pending.has_value()) {
                    ++result.metrics.intents_replaced;
                    pending.reset();
                }
            } else if (target.has_value() &&
                       (!pending.has_value() ||
                        pending->target_position != *target)) {
                if (pending.has_value()) {
                    ++result.metrics.intents_replaced;
                }
                pending = PendingIntent{
                    *target,
                    tick.sequence,
                    tick.timestamp_ns,
                    current_mid,
                };
                ++result.metrics.intents_submitted;
            }
        }

        const auto mark =
            mark_to_liquidation(
                portfolio, tick, config.execution,
                config.signal.max_order_quantity);
        all_liquidation_marks_available =
            all_liquidation_marks_available &&
            mark.modeled_liquidation_complete;
        result.equity_curve.push_back(
            EquityPoint{
                tick.sequence,
                tick.timestamp_ns,
                mark.gross_ticks,
                mark.net_ticks,
                portfolio.position,
            });

        if (mark.net_ticks >= peak_equity) {
            peak_equity = mark.net_ticks;
            current_drawdown_duration = 0U;
        } else {
            ++current_drawdown_duration;
            result.metrics.max_drawdown_ticks =
                std::max(
                    result.metrics.max_drawdown_ticks,
                    peak_equity - mark.net_ticks);
            result.metrics.max_drawdown_duration_events =
                std::max(
                    result.metrics
                        .max_drawdown_duration_events,
                    current_drawdown_duration);
        }
        const auto absolute_position =
            quantity_magnitude(portfolio.position);
        exposure_sum +=
            static_cast<long double>(absolute_position);
        if (portfolio.position != 0) {
            ++events_in_market;
        }
        if (absolute_position >
            static_cast<std::uint64_t>(
                result.metrics.maximum_absolute_position)) {
            result.metrics.maximum_absolute_position =
                static_cast<Quantity>(absolute_position);
        }

        const std::size_t processed = index - begin + 1U;
        if ((processed % config.metric_bucket_events) == 0U) {
            result.bucket_pnl_ticks.push_back(
                mark.net_ticks - previous_bucket_equity);
            previous_bucket_equity = mark.net_ticks;
        }
    }

    if (pending.has_value()) {
        ++result.metrics.cancelled_unfilled_intents;
    }

    const auto terminal =
        mark_to_liquidation(
            portfolio, ticks[end - 1U],
            config.execution,
            config.signal.max_order_quantity);
    result.metrics.gross_liquidation_pnl_ticks =
        terminal.gross_ticks;
    result.metrics.net_liquidation_pnl_ticks =
        terminal.net_ticks;
    result.metrics.executed_fill_shortfall_ticks =
        actual_fees + actual_spread_cost +
        actual_slippage_cost + actual_latency_cost;
    result.metrics.modeled_total_shortfall_ticks =
        terminal.gross_ticks - terminal.net_ticks;
    result.metrics.forced_liquidation_fill_shortfall_ticks =
        forced_liquidation_fill_shortfall;
    result.metrics.residual_modeled_liquidation_cost_ticks =
        terminal.terminal_fee_ticks +
        terminal.terminal_spread_ticks +
        terminal.terminal_slippage_ticks;
    result.metrics.fees_ticks = actual_fees;
    result.metrics.spread_cost_ticks =
        actual_spread_cost;
    result.metrics.slippage_cost_ticks =
        actual_slippage_cost;
    result.metrics.latency_cost_ticks =
        actual_latency_cost;
    result.metrics.ending_position = portfolio.position;
    result.metrics.all_liquidation_marks_available =
        all_liquidation_marks_available &&
        terminal.modeled_liquidation_complete;
    result.metrics.terminal_liquidation_complete =
        portfolio.position == 0;
    result.metrics.events =
        static_cast<std::uint64_t>(end - begin);
    const long double event_count =
        static_cast<long double>(result.metrics.events);
    result.metrics.average_absolute_exposure =
        exposure_sum / event_count;
    result.metrics.time_in_market_ratio =
        static_cast<long double>(events_in_market) /
        event_count;

    finalize_distribution_metrics(
        result.metrics, result.bucket_pnl_ticks);
    finalize_closed_trade_metrics(
        result.metrics, result.closed_trade_pnl_ticks);
    return result;
}

long double score_backtest(
    const BacktestMetrics& metrics,
    const std::uint64_t minimum_closed_trades) noexcept {
    if (metrics.closed_trades < minimum_closed_trades ||
        !metrics.terminal_liquidation_complete ||
        !metrics.all_liquidation_marks_available ||
        metrics.ending_position != 0 ||
        !std::isfinite(metrics.net_liquidation_pnl_ticks) ||
        !std::isfinite(metrics.max_drawdown_ticks) ||
        !std::isfinite(metrics.executed_fill_shortfall_ticks) ||
        !std::isfinite(metrics.average_absolute_exposure)) {
        return -std::numeric_limits<long double>::infinity();
    }
    const long double nonnegative_shortfall =
        std::max(0.0L,
                 metrics.executed_fill_shortfall_ticks);
    const long double conservative_utility =
        metrics.net_liquidation_pnl_ticks -
        metrics.max_drawdown_ticks -
        (0.25L * nonnegative_shortfall);
    const long double exposure_scale =
        std::max(1.0L, metrics.average_absolute_exposure);
    return conservative_utility / exposure_scale;
}

std::vector<BacktestConfig> default_backtest_candidates(
    const ExecutionCostConfig& execution) {
    validate_execution_config(execution);
    std::vector<BacktestConfig> candidates;
    candidates.reserve(11U);

    const auto add_trend =
        [&](const std::size_t fast,
            const std::size_t slow,
            const double threshold) {
            BacktestConfig config;
            config.execution = execution;
            config.signal.strategy =
                ResearchStrategy::Trend;
            config.signal.fast_window = fast;
            config.signal.slow_window = slow;
            config.signal.volatility_window = slow;
            config.signal.entry_threshold = threshold;
            config.signal.exit_threshold =
                threshold / 3.0;
            candidates.push_back(config);
        };
    add_trend(4U, 24U, 0.25);
    add_trend(8U, 48U, 0.30);
    add_trend(16U, 96U, 0.35);

    const auto add_mean_reversion =
        [&](const std::size_t window,
            const double threshold) {
            BacktestConfig config;
            config.execution = execution;
            config.signal.strategy =
                ResearchStrategy::MeanReversion;
            config.signal.fast_window = 8U;
            config.signal.slow_window = 48U;
            config.signal.mean_reversion_window = window;
            config.signal.volatility_window = window;
            config.signal.entry_threshold = threshold;
            config.signal.exit_threshold =
                threshold / 4.0;
            config.signal.max_holding_events =
                static_cast<std::uint64_t>(window * 4U);
            candidates.push_back(config);
        };
    add_mean_reversion(24U, 0.35);
    add_mean_reversion(48U, 0.40);
    add_mean_reversion(96U, 0.45);

    const auto add_imbalance =
        [&](const std::size_t window,
            const double threshold) {
            BacktestConfig config;
            config.execution = execution;
            config.signal.strategy =
                ResearchStrategy::OrderBookImbalance;
            config.signal.imbalance_window = window;
            config.signal.entry_threshold = threshold;
            config.signal.exit_threshold =
                threshold / 3.0;
            config.signal.rebalance_interval_events =
                static_cast<std::uint64_t>(
                    std::max<std::size_t>(2U, window / 2U));
            candidates.push_back(config);
        };
    add_imbalance(4U, 0.25);
    add_imbalance(12U, 0.35);

    const auto add_ensemble =
        [&](const double trend_weight,
            const double mean_weight,
            const double imbalance_weight) {
            BacktestConfig config;
            config.execution = execution;
            config.signal.strategy =
                ResearchStrategy::Ensemble;
            config.signal.trend_weight = trend_weight;
            config.signal.mean_reversion_weight = mean_weight;
            config.signal.imbalance_weight =
                imbalance_weight;
            candidates.push_back(config);
        };
    add_ensemble(0.50, 0.20, 0.30);
    add_ensemble(0.35, 0.35, 0.30);
    add_ensemble(0.30, 0.20, 0.50);
    return candidates;
}

WalkForwardReport run_walk_forward(
    const std::vector<MarketTick>& ticks,
    const std::vector<BacktestConfig>& candidates,
    const WalkForwardConfig& config) {
    if (candidates.empty() || candidates.size() > 128U) {
        throw std::invalid_argument(
            "walk-forward selection requires 1 to 128 candidates");
    }
    if (config.folds < 2U || config.folds > 10U ||
        config.final_test_percent < 10U ||
        config.final_test_percent > 40U ||
        config.minimum_closed_trades == 0U) {
        throw std::invalid_argument(
            "walk-forward folds, final test share, or trade floor is invalid");
    }
    const auto& reference_regime = candidates.front();
    for (const auto& candidate : candidates) {
        validate_signal_config(candidate.signal);
        validate_execution_config(candidate.execution);
        if (candidate.metric_bucket_events == 0U) {
            throw std::invalid_argument(
                "candidate metric bucket size must be positive");
        }
        if (!same_evaluation_regime(
                candidate, reference_regime)) {
            throw std::invalid_argument(
                "all walk-forward candidates must share execution, "
                "warmup, bucket, and liquidation-tail assumptions");
        }
    }
    validate_market_range(ticks, 0U, ticks.size());

    const std::size_t final_test_events =
        ((ticks.size() / 100U) *
         config.final_test_percent) +
        (((ticks.size() % 100U) *
          config.final_test_percent) /
         100U);
    const std::size_t final_test_begin =
        ticks.size() - final_test_events;
    if (final_test_begin <= config.embargo_events) {
        throw std::invalid_argument(
            "not enough data before the locked final test");
    }
    const std::size_t development_end =
        final_test_begin - config.embargo_events;
    const std::size_t initial_training_events =
        development_end / 2U;
    const std::size_t remaining =
        development_end - initial_training_events;
    if (config.embargo_events >
        std::numeric_limits<std::size_t>::max() /
            config.folds) {
        throw std::invalid_argument(
            "walk-forward embargo is too large");
    }
    const std::size_t total_fold_embargo =
        config.folds * config.embargo_events;
    if (remaining <= total_fold_embargo) {
        throw std::invalid_argument(
            "embargo leaves no walk-forward validation data");
    }
    const std::size_t validation_events =
        (remaining - total_fold_embargo) / config.folds;
    if (initial_training_events < 256U ||
        validation_events < 64U ||
        final_test_events < 128U) {
        throw std::invalid_argument(
            "backtest needs more events for training, folds, and test");
    }

    struct CandidateSummary {
        long double median_score{
            -std::numeric_limits<long double>::infinity()};
        long double worst_fold_score{
            -std::numeric_limits<long double>::infinity()};
        long double worst_fold_pnl{
            -std::numeric_limits<long double>::infinity()};
    };

    std::size_t selected_index = 0U;
    CandidateSummary selected_summary;
    bool selected = false;
    for (std::size_t candidate_index = 0U;
         candidate_index < candidates.size();
         ++candidate_index) {
        std::vector<long double> validation_scores;
        validation_scores.reserve(config.folds);
        long double worst_fold_score =
            std::numeric_limits<long double>::infinity();
        long double worst_fold_pnl =
            std::numeric_limits<long double>::infinity();
        bool all_folds_eligible = true;

        for (std::size_t fold = 0U;
             fold < config.folds; ++fold) {
            const std::size_t training_end =
                initial_training_events +
                (fold *
                 (validation_events +
                  config.embargo_events));
            const std::size_t validation_begin =
                training_end + config.embargo_events;
            const std::size_t validation_end =
                validation_begin + validation_events;
            const auto validation =
                run_backtest(
                    ticks, validation_begin,
                    validation_end,
                    candidates[candidate_index]);
            const long double fold_score =
                score_backtest(
                    validation.metrics,
                    config.minimum_closed_trades);
            validation_scores.push_back(fold_score);
            all_folds_eligible =
                all_folds_eligible &&
                std::isfinite(fold_score);
            worst_fold_score =
                std::min(worst_fold_score, fold_score);
            worst_fold_pnl =
                std::min(
                    worst_fold_pnl,
                    validation.metrics
                        .net_liquidation_pnl_ticks);
        }

        const CandidateSummary summary{
            all_folds_eligible
                ? median(std::move(validation_scores))
                : -std::numeric_limits<long double>::infinity(),
            all_folds_eligible
                ? worst_fold_score
                : -std::numeric_limits<long double>::infinity(),
            worst_fold_pnl,
        };
        const bool better =
            !selected ||
            summary.median_score >
                selected_summary.median_score ||
            (summary.median_score ==
                 selected_summary.median_score &&
             summary.worst_fold_score >
                 selected_summary.worst_fold_score);
        if (better) {
            selected = true;
            selected_index = candidate_index;
            selected_summary = summary;
        }
    }
    if (!selected ||
        !std::isfinite(selected_summary.median_score)) {
        throw std::runtime_error(
            "no candidate met the minimum closed-trade and liquidation "
            "requirements");
    }

    WalkForwardReport report;
    report.best_catalog_config = candidates[selected_index];
    report.walk_forward_config = config;
    report.final_test_begin = final_test_begin;
    report.candidates = candidates.size();
    report.candidate_fold_trials =
        candidates.size() * config.folds;
    report.selection_median_score =
        selected_summary.median_score;
    report.selection_worst_fold_score =
        selected_summary.worst_fold_score;
    report.selection_worst_fold_pnl_ticks =
        selected_summary.worst_fold_pnl;
    report.best_catalog_candidate_accepted =
        selected_summary.worst_fold_score > 0.0L;
    report.data_fingerprint =
        fingerprint_market_data(ticks);

    std::vector<BacktestResult> validation_results;
    validation_results.reserve(config.folds);
    report.best_catalog_candidate_folds.reserve(config.folds);
    for (std::size_t fold = 0U;
         fold < config.folds; ++fold) {
        const std::size_t training_end =
            initial_training_events +
            (fold *
             (validation_events +
              config.embargo_events));
        const std::size_t validation_begin =
            training_end + config.embargo_events;
        const std::size_t validation_end =
            validation_begin + validation_events;
        const auto training =
            run_backtest(
                ticks, 0U, training_end,
                report.best_catalog_config);
        auto validation =
            run_backtest(
                ticks, validation_begin,
                validation_end,
                report.best_catalog_config);
        if (validation.metrics
                .net_liquidation_pnl_ticks > 0.0L) {
            ++report.positive_net_pnl_validation_folds;
        }
        report.best_catalog_candidate_folds.push_back(
            WalkForwardFold{
                0U,
                training_end,
                validation_begin,
                validation_end,
                score_backtest(
                    training.metrics,
                    config.minimum_closed_trades),
                training.metrics,
                validation.metrics,
            });
        validation_results.push_back(
            std::move(validation));
    }
    report.best_catalog_aggregate_validation_metrics =
        aggregate_results(validation_results).metrics;

    const auto final_test =
        run_backtest(
            ticks, final_test_begin, ticks.size(),
            report.best_catalog_config);
    report.best_catalog_final_test_metrics =
        final_test.metrics;

    report.recommended_outcome_config =
        report.best_catalog_config;
    if (report.best_catalog_candidate_accepted) {
        report.recommended_outcome_final_test_metrics =
            final_test.metrics;
    } else {
        BacktestConfig no_trade;
        no_trade.signal.strategy =
            ResearchStrategy::NoTrade;
        no_trade.execution =
            report.best_catalog_config.execution;
        no_trade.metric_bucket_events =
            report.best_catalog_config.metric_bucket_events;
        no_trade.liquidation_tail_events =
            report.best_catalog_config.liquidation_tail_events;
        report.recommended_outcome_config = no_trade;
        report.recommended_outcome_final_test_metrics =
            run_backtest(
                ticks, final_test_begin, ticks.size(),
                no_trade)
                .metrics;
    }

    BacktestConfig scenario = report.best_catalog_config;
    scenario.execution =
        scenario_execution(scenario.execution);
    report.cost_latency_scenario_config = scenario;
    report.cost_latency_scenario_metrics =
        run_backtest(
            ticks, final_test_begin, ticks.size(),
            scenario)
            .metrics;

    BacktestConfig buy_and_hold;
    buy_and_hold.execution =
        report.best_catalog_config.execution;
    buy_and_hold.metric_bucket_events =
        report.best_catalog_config.metric_bucket_events;
    buy_and_hold.liquidation_tail_events =
        report.best_catalog_config.liquidation_tail_events;
    buy_and_hold.signal.strategy =
        ResearchStrategy::BuyAndHold;
    buy_and_hold.signal.rebalance_interval_events = 1U;
    buy_and_hold.warmup_events = 0U;
    report.buy_and_hold_config = buy_and_hold;
    report.buy_and_hold_final_test_metrics =
        run_backtest(
            ticks, final_test_begin, ticks.size(),
            buy_and_hold)
            .metrics;
    return report;
}

std::uint64_t fingerprint_market_data(
    const std::vector<MarketTick>& ticks) noexcept {
    constexpr std::uint64_t offset_basis =
        14'695'981'039'346'656'037ULL;
    constexpr std::uint64_t prime =
        1'099'511'628'211ULL;
    std::uint64_t hash = offset_basis;
    const auto mix_little_endian =
        [&](std::uint64_t value) {
            for (std::size_t byte = 0U;
                 byte < sizeof(value); ++byte) {
                hash ^= value & 0xffU;
                hash *= prime;
                value >>= 8U;
            }
        };
    for (const auto& tick : ticks) {
        mix_little_endian(tick.sequence);
        mix_little_endian(tick.timestamp_ns);
        mix_little_endian(
            static_cast<std::uint64_t>(tick.bid_ticks));
        mix_little_endian(
            static_cast<std::uint64_t>(tick.bid_quantity));
        mix_little_endian(
            static_cast<std::uint64_t>(tick.ask_ticks));
        mix_little_endian(
            static_cast<std::uint64_t>(tick.ask_quantity));
    }
    return hash;
}

}  // namespace stockagent
