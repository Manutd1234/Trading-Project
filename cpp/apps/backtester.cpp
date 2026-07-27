#include "stockagent/backtest.hpp"
#include "stockagent/csv_replay.hpp"
#include "stockagent/synthetic_feed.hpp"
#include "stockagent/types.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Options {
    std::size_t events{40'000};
    std::uint64_t seed{42};
    std::string csv_path;
    std::string strategy{"auto"};
    stockagent::WalkForwardConfig walk_forward{};
    stockagent::ExecutionCostConfig execution{};
    std::size_t metric_bucket_events{64};
    std::size_t liquidation_tail_events{32};
    bool json{false};
};

[[nodiscard]] std::uint64_t parse_unsigned(
    const std::string_view text, const std::string_view name) {
    std::uint64_t value{};
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        throw std::invalid_argument(
            "invalid value for " + std::string(name));
    }
    return value;
}

[[nodiscard]] double parse_double(
    const std::string_view text, const std::string_view name) {
    const std::string owned(text);
    char* parsed_end = nullptr;
    const double value =
        std::strtod(owned.c_str(), &parsed_end);
    if (owned.empty() || parsed_end == owned.c_str() ||
        parsed_end != owned.c_str() + owned.size() ||
        !std::isfinite(value)) {
        throw std::invalid_argument(
            "invalid value for " + std::string(name));
    }
    return value;
}

template <typename Target>
[[nodiscard]] Target bounded_unsigned(
    const std::string_view text, const std::string_view name,
    const std::uint64_t minimum,
    const std::uint64_t maximum) {
    const auto value = parse_unsigned(text, name);
    if (value < minimum || value > maximum) {
        throw std::invalid_argument(
            std::string(name) + " is outside its supported range");
    }
    return static_cast<Target>(value);
}

void print_help() {
    std::cout
        << "StockAgent causal walk-forward backtester\n\n"
        << "Usage: stockagent_backtester [options]\n\n"
        << "Data:\n"
        << "  --events N                 Synthetic event count\n"
        << "  --seed N                   Synthetic feed seed\n"
        << "  --csv PATH                 Ordered top-of-book CSV\n\n"
        << "Research design:\n"
        << "  --strategy NAME            auto, trend, mean-reversion,\n"
        << "                             imbalance, or ensemble\n"
        << "  --folds N                  Expanding validation folds\n"
        << "  --final-test-percent N     Locked chronological test share\n"
        << "  --embargo-events N         Gap before folds and final test\n"
        << "  --minimum-closed-trades N  Candidate eligibility floor\n"
        << "  --metric-bucket-events N   Event-count P&L bucket\n"
        << "  --liquidation-tail-events N  Reserved close-only events\n\n"
        << "Execution assumptions:\n"
        << "  --fixed-fee-ticks N\n"
        << "  --fee-ticks-per-unit N\n"
        << "  --base-slippage-ticks N\n"
        << "  --impact-coefficient N\n"
        << "  --max-participation-bps N\n"
        << "  --latency-ns N\n\n"
        << "Output:\n"
        << "  --json                     Emit machine-readable JSON\n"
        << "  --help                     Show this help\n";
}

[[nodiscard]] Options parse_options(
    const int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg(argv[index]);
        const auto value = [&]() -> std::string_view {
            if ((index + 1) >= argc) {
                throw std::invalid_argument(
                    "missing value for " + std::string(arg));
            }
            ++index;
            return argv[index];
        };

        if (arg == "--events") {
            options.events =
                bounded_unsigned<std::size_t>(
                    value(), arg, 2'000U, 2'000'000U);
        } else if (arg == "--seed") {
            options.seed = parse_unsigned(value(), arg);
        } else if (arg == "--csv") {
            options.csv_path = value();
        } else if (arg == "--strategy") {
            options.strategy = value();
        } else if (arg == "--folds") {
            options.walk_forward.folds =
                bounded_unsigned<std::size_t>(
                    value(), arg, 2U, 10U);
        } else if (arg == "--final-test-percent") {
            options.walk_forward.final_test_percent =
                bounded_unsigned<std::size_t>(
                    value(), arg, 10U, 40U);
        } else if (arg == "--embargo-events") {
            options.walk_forward.embargo_events =
                bounded_unsigned<std::size_t>(
                    value(), arg, 0U, 1'000'000U);
        } else if (arg == "--minimum-closed-trades") {
            options.walk_forward.minimum_closed_trades =
                bounded_unsigned<std::uint64_t>(
                    value(), arg, 1U, 1'000'000U);
        } else if (arg == "--metric-bucket-events") {
            options.metric_bucket_events =
                bounded_unsigned<std::size_t>(
                    value(), arg, 1U, 1'000'000U);
        } else if (arg == "--liquidation-tail-events") {
            options.liquidation_tail_events =
                bounded_unsigned<std::size_t>(
                    value(), arg, 1U, 1'000'000U);
        } else if (arg == "--fixed-fee-ticks") {
            options.execution.fixed_fee_ticks =
                bounded_unsigned<std::int64_t>(
                    value(), arg, 0U,
                    static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max()));
        } else if (arg == "--fee-ticks-per-unit") {
            options.execution.fee_ticks_per_unit =
                bounded_unsigned<std::int64_t>(
                    value(), arg, 0U,
                    static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max()));
        } else if (arg == "--base-slippage-ticks") {
            options.execution.base_slippage_ticks =
                bounded_unsigned<stockagent::Price>(
                    value(), arg, 0U,
                    static_cast<std::uint64_t>(
                        std::numeric_limits<
                            stockagent::Price>::max()));
        } else if (arg == "--impact-coefficient") {
            options.execution.impact_coefficient_ticks =
                parse_double(value(), arg);
        } else if (arg == "--max-participation-bps") {
            options.execution.max_participation_bps =
                bounded_unsigned<std::uint32_t>(
                    value(), arg, 1U, 10'000U);
        } else if (arg == "--latency-ns") {
            options.execution.latency_ns =
                parse_unsigned(value(), arg);
        } else if (arg == "--json") {
            options.json = true;
        } else if (arg == "--help" || arg == "-h") {
            print_help();
            std::exit(0);
        } else {
            throw std::invalid_argument(
                "unknown option: " + std::string(arg));
        }
    }

    if (options.strategy != "auto" &&
        options.strategy != "trend" &&
        options.strategy != "mean-reversion" &&
        options.strategy != "imbalance" &&
        options.strategy != "ensemble") {
        throw std::invalid_argument(
            "--strategy must be auto, trend, mean-reversion, "
            "imbalance, or ensemble");
    }
    return options;
}

[[nodiscard]] std::vector<stockagent::MarketTick>
make_synthetic(const std::size_t count,
               const std::uint64_t seed) {
    std::vector<stockagent::MarketTick> ticks;
    ticks.reserve(count);
    stockagent::SyntheticFeed feed(seed);
    for (std::size_t index = 0; index < count; ++index) {
        ticks.push_back(feed.next());
    }
    return ticks;
}

[[nodiscard]] bool selected_strategy(
    const stockagent::ResearchStrategy candidate,
    const std::string& requested) {
    if (requested == "auto") {
        return true;
    }
    if (requested == "trend") {
        return candidate ==
               stockagent::ResearchStrategy::Trend;
    }
    if (requested == "mean-reversion") {
        return candidate ==
               stockagent::ResearchStrategy::MeanReversion;
    }
    if (requested == "imbalance") {
        return candidate ==
               stockagent::ResearchStrategy::OrderBookImbalance;
    }
    return candidate ==
           stockagent::ResearchStrategy::Ensemble;
}

[[nodiscard]] std::vector<stockagent::BacktestConfig>
candidate_set(const Options& options) {
    const auto all =
        stockagent::default_backtest_candidates(
            options.execution);
    std::vector<stockagent::BacktestConfig> selected;
    for (auto candidate : all) {
        if (selected_strategy(
                candidate.signal.strategy,
                options.strategy)) {
            candidate.metric_bucket_events =
                options.metric_bucket_events;
            candidate.liquidation_tail_events =
                options.liquidation_tail_events;
            selected.push_back(candidate);
        }
    }
    if (selected.empty()) {
        throw std::runtime_error(
            "strategy filter produced no candidates");
    }
    return selected;
}

[[nodiscard]] std::size_t effective_warmup_events(
    const stockagent::BacktestConfig& config) {
    const stockagent::ResearchSignalModel model(config.signal);
    return std::max(
        config.warmup_events,
        model.required_warmup_events());
}

void print_metric_value(const std::string_view name,
                        const long double value) {
    std::cout << name << ": "
              << static_cast<double>(value) << '\n';
}

void print_metrics(
    const std::string_view label,
    const stockagent::BacktestMetrics& metrics) {
    std::cout << '\n' << label << '\n';
    print_metric_value(
        "gross_liquidation_pnl_ticks",
        metrics.gross_liquidation_pnl_ticks);
    print_metric_value(
        "net_liquidation_pnl_ticks",
        metrics.net_liquidation_pnl_ticks);
    print_metric_value(
        "executed_fill_shortfall_ticks",
        metrics.executed_fill_shortfall_ticks);
    print_metric_value(
        "modeled_total_shortfall_ticks",
        metrics.modeled_total_shortfall_ticks);
    print_metric_value(
        "forced_liquidation_fill_shortfall_ticks",
        metrics.forced_liquidation_fill_shortfall_ticks);
    print_metric_value(
        "residual_modeled_liquidation_cost_ticks",
        metrics.residual_modeled_liquidation_cost_ticks);
    print_metric_value(
        "executed_fees_ticks", metrics.fees_ticks);
    print_metric_value(
        "executed_spread_cost_ticks",
        metrics.spread_cost_ticks);
    print_metric_value(
        "executed_slippage_cost_ticks",
        metrics.slippage_cost_ticks);
    print_metric_value(
        "executed_latency_cost_ticks",
        metrics.latency_cost_ticks);
    print_metric_value(
        "max_drawdown_ticks", metrics.max_drawdown_ticks);
    std::cout << "max_drawdown_duration_events: "
              << metrics.max_drawdown_duration_events << '\n';
    if (metrics.sharpe_available) {
        print_metric_value(
            "unadjusted_sharpe_per_sqrt_event_bucket",
            metrics.sharpe_per_sqrt_bucket);
    } else {
        std::cout
            << "unadjusted_sharpe_per_sqrt_event_bucket: N/A\n";
    }
    if (metrics.sortino_available) {
        print_metric_value(
            "unadjusted_sortino_per_sqrt_event_bucket",
            metrics.sortino_per_sqrt_bucket);
    } else {
        std::cout
            << "unadjusted_sortino_per_sqrt_event_bucket: N/A\n";
    }
    if (metrics.tail_metric_available) {
        print_metric_value(
            "mean_worst_5pct_event_bucket_pnl_ticks",
            metrics.mean_worst_5pct_bucket_pnl_ticks);
    } else {
        std::cout
            << "mean_worst_5pct_event_bucket_pnl_ticks: N/A\n";
    }
    if (metrics.metric_buckets > 0U) {
        print_metric_value(
            "mean_bucket_pnl_ticks",
            metrics.mean_bucket_pnl_ticks);
        print_metric_value(
            "downside_deviation_ticks",
            metrics.downside_deviation_ticks);
        print_metric_value(
            "worst_bucket_pnl_ticks",
            metrics.worst_bucket_pnl_ticks);
    } else {
        std::cout << "mean_bucket_pnl_ticks: N/A\n"
                  << "downside_deviation_ticks: N/A\n"
                  << "worst_bucket_pnl_ticks: N/A\n";
    }
    if (metrics.metric_buckets >= 2U) {
        print_metric_value(
            "bucket_volatility_ticks",
            metrics.bucket_volatility_ticks);
    } else {
        std::cout << "bucket_volatility_ticks: N/A\n";
    }
    print_metric_value(
        "quantity_turnover", metrics.quantity_turnover);
    print_metric_value(
        "average_absolute_position_quantity",
        metrics.average_absolute_exposure);
    print_metric_value(
        "event_weighted_time_in_market_ratio",
        metrics.time_in_market_ratio);
    if (metrics.closed_trades > 0U) {
        print_metric_value(
            "closed_trade_hit_rate",
            metrics.closed_trade_hit_rate);
        print_metric_value(
            "average_closed_trade_pnl_ticks",
            metrics.average_closed_trade_pnl_ticks);
    } else {
        std::cout << "closed_trade_hit_rate: N/A\n"
                  << "average_closed_trade_pnl_ticks: N/A\n";
    }
    if (metrics.closed_trades > 0U &&
        std::isfinite(metrics.closed_trade_profit_factor)) {
        print_metric_value(
            "closed_trade_profit_factor",
            metrics.closed_trade_profit_factor);
    } else {
        std::cout
            << "closed_trade_profit_factor: N/A\n";
    }
    std::cout << "events: " << metrics.events << '\n'
              << "metric_buckets: "
              << metrics.metric_buckets << '\n'
              << "intents_submitted: "
              << metrics.intents_submitted << '\n'
              << "intents_replaced: "
              << metrics.intents_replaced << '\n'
              << "fills: " << metrics.fills << '\n'
              << "partial_or_capacity_limited_fills: "
              << metrics
                     .partial_or_capacity_limited_fills
              << '\n'
              << "cancelled_unfilled_intents: "
              << metrics.cancelled_unfilled_intents << '\n'
              << "closed_trades: "
              << metrics.closed_trades << '\n'
              << "ending_position: "
              << metrics.ending_position << '\n'
              << "all_liquidation_marks_available: "
              << (metrics.all_liquidation_marks_available
                      ? "true"
                      : "false")
              << '\n'
              << "terminal_liquidation_complete: "
              << (metrics.terminal_liquidation_complete
                      ? "true"
                      : "false")
              << '\n'
              << "causal_comparison_valid: "
              << (metrics.terminal_liquidation_complete &&
                          metrics.all_liquidation_marks_available
                      ? "true"
                      : "false")
              << '\n';
}

void print_human(
    const Options& options,
    const std::vector<stockagent::MarketTick>& ticks,
    const stockagent::WalkForwardReport& report) {
    std::cout
        << std::fixed << std::setprecision(4)
        << "StockAgent causal systematic-strategy research backtest\n"
        << "Public research models; not a reproduction of any firm's "
           "proprietary strategy.\n\n"
        << "report_schema_version: 2\n"
        << "code_version: 1.1.0\n"
        << "source: "
        << (options.csv_path.empty()
                ? "synthetic_pipeline_demo"
                : options.csv_path)
        << '\n'
        << "synthetic_seed: "
        << (options.csv_path.empty()
                ? std::to_string(options.seed)
                : std::string("N/A"))
        << '\n'
        << "events: " << ticks.size() << '\n'
        << "data_fingerprint_fnv1a64: " << std::hex
        << report.data_fingerprint << std::dec << '\n'
        << "bounded_catalog_candidates: "
        << report.candidates << '\n'
        << "programmatic_candidate_fold_evaluations: "
        << report.candidate_fold_trials << '\n'
        << "walk_forward_folds: "
        << report.walk_forward_config.folds << '\n'
        << "final_test_percent: "
        << report.walk_forward_config.final_test_percent
        << '\n'
        << "embargo_events: "
        << report.walk_forward_config.embargo_events
        << '\n'
        << "minimum_closed_trades_per_validation_fold: "
        << report.walk_forward_config.minimum_closed_trades
        << '\n'
        << "final_test_begin: "
        << report.final_test_begin << '\n'
        << "recommended_outcome: "
        << stockagent::to_string(
               report.recommended_outcome_config.signal.strategy)
        << '\n'
        << "best_catalog_strategy: "
        << stockagent::to_string(
               report.best_catalog_config.signal.strategy)
        << '\n'
        << "best_catalog_fast_window: "
        << report.best_catalog_config.signal.fast_window << '\n'
        << "best_catalog_slow_window: "
        << report.best_catalog_config.signal.slow_window << '\n'
        << "best_catalog_mean_reversion_window: "
        << report.best_catalog_config.signal
               .mean_reversion_window
        << '\n'
        << "best_catalog_volatility_window: "
        << report.best_catalog_config.signal
               .volatility_window
        << '\n'
        << "best_catalog_imbalance_window: "
        << report.best_catalog_config.signal
               .imbalance_window
        << '\n'
        << "best_catalog_volatility_target_ticks: "
        << report.best_catalog_config.signal
               .volatility_target_ticks
        << '\n'
        << "best_catalog_entry_threshold: "
        << report.best_catalog_config.signal.entry_threshold
        << '\n'
        << "best_catalog_exit_threshold: "
        << report.best_catalog_config.signal.exit_threshold
        << '\n'
        << "best_catalog_trend_weight: "
        << report.best_catalog_config.signal.trend_weight
        << '\n'
        << "best_catalog_mean_reversion_weight: "
        << report.best_catalog_config.signal
               .mean_reversion_weight
        << '\n'
        << "best_catalog_imbalance_weight: "
        << report.best_catalog_config.signal
               .imbalance_weight
        << '\n'
        << "best_catalog_mean_reversion_trend_filter: "
        << report.best_catalog_config.signal
               .mean_reversion_trend_filter
        << '\n'
        << "best_catalog_base_target_quantity: "
        << report.best_catalog_config.signal
               .base_target_quantity
        << '\n'
        << "best_catalog_max_absolute_position: "
        << report.best_catalog_config.signal
               .max_absolute_position
        << '\n'
        << "best_catalog_max_order_quantity: "
        << report.best_catalog_config.signal
               .max_order_quantity
        << '\n'
        << "best_catalog_rebalance_interval_events: "
        << report.best_catalog_config.signal
               .rebalance_interval_events
        << '\n'
        << "best_catalog_max_holding_events: "
        << report.best_catalog_config.signal
               .max_holding_events
        << '\n'
        << "best_catalog_configured_minimum_warmup_events: "
        << report.best_catalog_config.warmup_events << '\n'
        << "best_catalog_effective_warmup_requirement_events: "
        << effective_warmup_events(
               report.best_catalog_config)
        << '\n'
        << "selection_median_validation_score: "
        << static_cast<double>(
               report.selection_median_score)
        << '\n'
        << "selection_worst_fold_score: "
        << static_cast<double>(
               report.selection_worst_fold_score)
        << '\n'
        << "selection_worst_fold_pnl_ticks: "
        << static_cast<double>(
               report.selection_worst_fold_pnl_ticks)
        << '\n'
        << "positive_net_pnl_validation_folds: "
        << report.positive_net_pnl_validation_folds << '/'
        << report.best_catalog_candidate_folds.size() << '\n'
        << "best_catalog_candidate_accepted: "
        << (report.best_catalog_candidate_accepted
                ? "true"
                : "false")
        << '\n'
        << "latency_ns: "
        << report.best_catalog_config.execution.latency_ns
        << '\n'
        << "max_participation_bps: "
        << report.best_catalog_config.execution
               .max_participation_bps
        << '\n'
        << "fixed_fee_ticks: "
        << report.best_catalog_config.execution
               .fixed_fee_ticks
        << '\n'
        << "fee_ticks_per_unit: "
        << report.best_catalog_config.execution
               .fee_ticks_per_unit
        << '\n'
        << "base_slippage_ticks: "
        << report.best_catalog_config.execution
               .base_slippage_ticks
        << '\n'
        << "impact_coefficient_ticks: "
        << report.best_catalog_config.execution
               .impact_coefficient_ticks
        << '\n'
        << "metric_bucket_events: "
        << report.best_catalog_config.metric_bucket_events
        << '\n'
        << "liquidation_tail_events: "
        << report.best_catalog_config.liquidation_tail_events
        << '\n';

    std::cout
        << "\nfold  train_range  validation_range  "
           "train_score  validation_net  validation_dd  fills\n";
    for (std::size_t index = 0;
         index < report.best_catalog_candidate_folds.size();
         ++index) {
        const auto& fold =
            report.best_catalog_candidate_folds[index];
        std::cout
            << (index + 1U) << "  ["
            << fold.training_begin << ','
            << fold.training_end << ")  ["
            << fold.validation_begin << ','
            << fold.validation_end << ")  "
            << (std::isfinite(fold.training_score)
                    ? std::to_string(
                          static_cast<double>(
                              fold.training_score))
                    : std::string("N/A"))
            << "  "
            << static_cast<double>(
                   fold.validation_metrics
                       .net_liquidation_pnl_ticks)
            << "  "
            << static_cast<double>(
                   fold.validation_metrics
                       .max_drawdown_ticks)
            << "  "
            << fold.validation_metrics.fills << '\n';
    }

    print_metrics(
        "best_catalog_aggregate_walk_forward_validation",
        report.best_catalog_aggregate_validation_metrics);
    print_metrics(
        "recommended_outcome_locked_final_test",
        report.recommended_outcome_final_test_metrics);
    if (!report.best_catalog_candidate_accepted) {
        print_metrics(
            "rejected_best_catalog_candidate_locked_final_test",
            report.best_catalog_final_test_metrics);
    }
    print_metrics(
        "best_catalog_locked_final_test_2x_cost_and_latency_scenario",
        report.cost_latency_scenario_metrics);
    print_metrics(
        "locked_final_test_buy_and_hold_baseline",
        report.buy_and_hold_final_test_metrics);

    std::cout
        << "\nNo-trade baseline net P&L and selection score are 0. "
           "The best catalog candidate is recommended only when every "
           "validation-fold score is above zero; otherwise the recommended "
           "outcome is no_trade and candidate final-test results are "
           "diagnostic only. "
           "Signals are generated at t and can fill only on a later "
           "market event after configured latency. Synthetic results "
           "validate plumbing only; use point-in-time historical data. "
           "Viewing and then reusing the locked test for selection leaks "
           "it. Top-of-book replay cannot establish queue priority, "
           "real capacity, or future profitability. Compare P&L only when "
           "causal_comparison_valid is true. Executed-fill shortfall omits "
           "the opportunity cost of unfilled targets.\n";
}

void print_json_number(
    const std::string_view key, const long double value,
    const bool trailing_comma = true) {
    std::cout << '"' << key << "\":";
    if (std::isfinite(value)) {
        std::cout << static_cast<double>(value);
    } else {
        std::cout << "null";
    }
    if (trailing_comma) {
        std::cout << ',';
    }
}

void print_json_metrics(
    const stockagent::BacktestMetrics& metrics) {
    std::cout << '{';
    print_json_number(
        "gross_liquidation_pnl_ticks",
        metrics.gross_liquidation_pnl_ticks);
    print_json_number(
        "net_liquidation_pnl_ticks",
        metrics.net_liquidation_pnl_ticks);
    print_json_number(
        "executed_fill_shortfall_ticks",
        metrics.executed_fill_shortfall_ticks);
    print_json_number(
        "modeled_total_shortfall_ticks",
        metrics.modeled_total_shortfall_ticks);
    print_json_number(
        "forced_liquidation_fill_shortfall_ticks",
        metrics.forced_liquidation_fill_shortfall_ticks);
    print_json_number(
        "residual_modeled_liquidation_cost_ticks",
        metrics.residual_modeled_liquidation_cost_ticks);
    print_json_number(
        "executed_fees_ticks", metrics.fees_ticks);
    print_json_number(
        "executed_spread_cost_ticks",
        metrics.spread_cost_ticks);
    print_json_number(
        "executed_slippage_cost_ticks",
        metrics.slippage_cost_ticks);
    print_json_number(
        "executed_latency_cost_ticks",
        metrics.latency_cost_ticks);
    print_json_number(
        "max_drawdown_ticks", metrics.max_drawdown_ticks);
    print_json_number(
        "unadjusted_sharpe_per_sqrt_event_bucket",
        metrics.sharpe_available
            ? metrics.sharpe_per_sqrt_bucket
            : std::numeric_limits<long double>::quiet_NaN());
    print_json_number(
        "unadjusted_sortino_per_sqrt_event_bucket",
        metrics.sortino_available
            ? metrics.sortino_per_sqrt_bucket
            : std::numeric_limits<long double>::quiet_NaN());
    print_json_number(
        "mean_worst_5pct_event_bucket_pnl_ticks",
        metrics.tail_metric_available
            ? metrics.mean_worst_5pct_bucket_pnl_ticks
            : std::numeric_limits<long double>::quiet_NaN());
    print_json_number(
        "mean_bucket_pnl_ticks",
        metrics.metric_buckets > 0U
            ? metrics.mean_bucket_pnl_ticks
            : std::numeric_limits<long double>::quiet_NaN());
    print_json_number(
        "bucket_volatility_ticks",
        metrics.metric_buckets >= 2U
            ? metrics.bucket_volatility_ticks
            : std::numeric_limits<long double>::quiet_NaN());
    print_json_number(
        "downside_deviation_ticks",
        metrics.metric_buckets > 0U
            ? metrics.downside_deviation_ticks
            : std::numeric_limits<long double>::quiet_NaN());
    print_json_number(
        "worst_bucket_pnl_ticks",
        metrics.metric_buckets > 0U
            ? metrics.worst_bucket_pnl_ticks
            : std::numeric_limits<long double>::quiet_NaN());
    print_json_number(
        "quantity_turnover", metrics.quantity_turnover);
    print_json_number(
        "average_absolute_position_quantity",
        metrics.average_absolute_exposure);
    print_json_number(
        "event_weighted_time_in_market_ratio",
        metrics.time_in_market_ratio);
    print_json_number(
        "closed_trade_hit_rate",
        metrics.closed_trades > 0U
            ? metrics.closed_trade_hit_rate
            : std::numeric_limits<long double>::quiet_NaN());
    print_json_number(
        "closed_trade_profit_factor",
        metrics.closed_trades > 0U
            ? metrics.closed_trade_profit_factor
            : std::numeric_limits<long double>::quiet_NaN());
    print_json_number(
        "average_closed_trade_pnl_ticks",
        metrics.closed_trades > 0U
            ? metrics.average_closed_trade_pnl_ticks
            : std::numeric_limits<long double>::quiet_NaN());
    std::cout
              << "\"max_drawdown_duration_events\":"
              << metrics.max_drawdown_duration_events << ','
              << "\"events\":" << metrics.events << ','
              << "\"metric_buckets\":"
              << metrics.metric_buckets << ','
              << "\"intents_submitted\":"
              << metrics.intents_submitted << ','
              << "\"intents_replaced\":"
              << metrics.intents_replaced << ','
              << "\"fills\":" << metrics.fills << ','
              << "\"partial_or_capacity_limited_fills\":"
              << metrics.partial_or_capacity_limited_fills
              << ','
              << "\"cancelled_unfilled_intents\":"
              << metrics.cancelled_unfilled_intents << ','
              << "\"closed_trades\":"
              << metrics.closed_trades << ','
              << "\"winning_closed_trades\":"
              << metrics.winning_closed_trades << ','
              << "\"ending_position\":"
              << metrics.ending_position << ','
              << "\"maximum_absolute_position\":"
              << metrics.maximum_absolute_position << ','
              << "\"sharpe_available\":"
              << (metrics.sharpe_available
                      ? "true"
                      : "false")
              << ','
              << "\"sortino_available\":"
              << (metrics.sortino_available
                      ? "true"
                      : "false")
              << ','
              << "\"tail_metric_available\":"
              << (metrics.tail_metric_available
                      ? "true"
                      : "false")
              << ','
              << "\"all_liquidation_marks_available\":"
              << (metrics.all_liquidation_marks_available
                      ? "true"
                      : "false")
              << ','
              << "\"terminal_liquidation_complete\":"
              << (metrics.terminal_liquidation_complete
                      ? "true"
                      : "false")
              << ','
              << "\"causal_comparison_valid\":"
              << (metrics.terminal_liquidation_complete &&
                          metrics.all_liquidation_marks_available
                      ? "true"
                      : "false")
              << '}';
}

void print_json_config(
    const stockagent::BacktestConfig& config) {
    std::cout
        << "{\"signal\":{\"strategy\":\""
        << stockagent::to_string(config.signal.strategy)
        << "\",\"fast_window\":"
        << config.signal.fast_window
        << ",\"slow_window\":"
        << config.signal.slow_window
        << ",\"mean_reversion_window\":"
        << config.signal.mean_reversion_window
        << ",\"volatility_window\":"
        << config.signal.volatility_window
        << ",\"imbalance_window\":"
        << config.signal.imbalance_window << ',';
    print_json_number(
        "volatility_target_ticks",
        config.signal.volatility_target_ticks);
    print_json_number(
        "entry_threshold",
        config.signal.entry_threshold);
    print_json_number(
        "exit_threshold",
        config.signal.exit_threshold);
    print_json_number(
        "trend_weight",
        config.signal.trend_weight);
    print_json_number(
        "mean_reversion_weight",
        config.signal.mean_reversion_weight);
    print_json_number(
        "imbalance_weight",
        config.signal.imbalance_weight);
    print_json_number(
        "mean_reversion_trend_filter",
        config.signal.mean_reversion_trend_filter);
    std::cout
        << "\"base_target_quantity\":"
        << config.signal.base_target_quantity
        << ",\"max_absolute_position\":"
        << config.signal.max_absolute_position
        << ",\"max_order_quantity\":"
        << config.signal.max_order_quantity
        << ",\"rebalance_interval_events\":"
        << config.signal.rebalance_interval_events
        << ",\"max_holding_events\":"
        << config.signal.max_holding_events
        << "},\"execution\":{\"fixed_fee_ticks\":"
        << config.execution.fixed_fee_ticks
        << ",\"fee_ticks_per_unit\":"
        << config.execution.fee_ticks_per_unit
        << ",\"base_slippage_ticks\":"
        << config.execution.base_slippage_ticks << ',';
    print_json_number(
        "impact_coefficient_ticks",
        config.execution.impact_coefficient_ticks);
    std::cout
        << "\"max_participation_bps\":"
        << config.execution.max_participation_bps
        << ",\"latency_ns\":"
        << config.execution.latency_ns
        << "},\"evaluation\":{"
           "\"configured_minimum_warmup_events\":"
        << config.warmup_events
        << ",\"effective_warmup_requirement_events\":"
        << effective_warmup_events(config)
        << ",\"metric_bucket_events\":"
        << config.metric_bucket_events
        << ",\"liquidation_tail_events\":"
        << config.liquidation_tail_events
        << "}}";
}

void print_json(
    const Options& options,
    const std::vector<stockagent::MarketTick>& ticks,
    const stockagent::WalkForwardReport& report) {
    std::cout
        << std::fixed << std::setprecision(8)
        << "{\"schema_version\":2,"
        << "\"code_version\":\"1.1.0\","
        << "\"research_design\":"
           "\"causal_chronological_multi_window_selection\","
        << "\"input_mode\":\""
        << (options.csv_path.empty()
                ? "synthetic_pipeline_demo"
                : "csv")
        << "\",\"synthetic_seed\":";
    if (options.csv_path.empty()) {
        std::cout << options.seed;
    } else {
        std::cout << "null";
    }
    std::cout
        << ",\"events\":" << ticks.size()
        << ",\"data_fingerprint_fnv1a64\":\""
        << std::hex << report.data_fingerprint
        << std::dec << "\",\"bounded_catalog_candidates\":"
        << report.candidates
        << ",\"programmatic_candidate_fold_evaluations\":"
        << report.candidate_fold_trials
        << ",\"walk_forward\":{\"folds\":"
        << report.walk_forward_config.folds
        << ",\"final_test_percent\":"
        << report.walk_forward_config.final_test_percent
        << ",\"embargo_events\":"
        << report.walk_forward_config.embargo_events
        << ",\"minimum_closed_trades_per_validation_fold\":"
        << report.walk_forward_config.minimum_closed_trades
        << "},\"final_test_begin\":"
        << report.final_test_begin
        << ",\"recommended_outcome\":\""
        << stockagent::to_string(
               report.recommended_outcome_config.signal.strategy)
        << "\",\"best_catalog_strategy\":\""
        << stockagent::to_string(
               report.best_catalog_config.signal.strategy)
        << "\",\"recommended_outcome_config\":";
    print_json_config(
        report.recommended_outcome_config);
    std::cout << ",\"best_catalog_config\":";
    print_json_config(report.best_catalog_config);
    std::cout << ",\"selection\":{";
    print_json_number(
        "median_validation_score",
        report.selection_median_score);
    print_json_number(
        "worst_fold_score",
        report.selection_worst_fold_score);
    print_json_number(
        "worst_fold_net_pnl_ticks",
        report.selection_worst_fold_pnl_ticks);
    std::cout
        << "\"positive_net_pnl_validation_folds\":"
        << report.positive_net_pnl_validation_folds
        << ",\"total_validation_folds\":"
        << report.best_catalog_candidate_folds.size()
        << ",\"best_catalog_candidate_accepted\":"
        << (report.best_catalog_candidate_accepted
                ? "true"
                : "false")
        << "},\"no_trade_baseline\":{"
           "\"net_liquidation_pnl_ticks\":0,"
           "\"selection_score\":0},\"folds\":[";
    for (std::size_t index = 0;
         index < report.best_catalog_candidate_folds.size();
         ++index) {
        if (index != 0U) {
            std::cout << ',';
        }
        const auto& fold =
            report.best_catalog_candidate_folds[index];
        std::cout
            << "{\"training_begin\":"
            << fold.training_begin
            << ",\"training_end\":"
            << fold.training_end
            << ",\"validation_begin\":"
            << fold.validation_begin
            << ",\"validation_end\":"
            << fold.validation_end << ',';
        print_json_number(
            "training_score", fold.training_score);
        std::cout << "\"training_metrics\":";
        print_json_metrics(fold.training_metrics);
        std::cout << ",\"validation_metrics\":";
        print_json_metrics(fold.validation_metrics);
        std::cout << '}';
    }
    std::cout
        << "],\"best_catalog_aggregate_validation\":";
    print_json_metrics(
        report.best_catalog_aggregate_validation_metrics);
    std::cout
        << ",\"recommended_outcome_final_test\":";
    print_json_metrics(
        report.recommended_outcome_final_test_metrics);
    std::cout
        << ",\"best_catalog_candidate_final_test\":";
    print_json_metrics(
        report.best_catalog_final_test_metrics);
    std::cout
        << ",\"two_x_cost_and_latency_scenario\":{"
           "\"config\":";
    print_json_config(
        report.cost_latency_scenario_config);
    std::cout << ",\"metrics\":";
    print_json_metrics(
        report.cost_latency_scenario_metrics);
    std::cout
        << "},\"buy_and_hold_baseline\":{"
           "\"risk_or_volatility_matched\":false,\"config\":";
    print_json_config(report.buy_and_hold_config);
    std::cout << ",\"metrics\":";
    print_json_metrics(
        report.buy_and_hold_final_test_metrics);
    std::cout
        << "},\"warnings\":["
        << "\"public research models, not proprietary firm strategies\","
        << "\"synthetic data is a pipeline demo only\","
        << "\"top-of-book replay does not model queue priority or real capacity\","
        << "\"executed-fill shortfall omits unfilled-target opportunity cost\","
        << "\"the two-times cost-and-latency scenario is a sensitivity case, not guaranteed to reduce P&L\","
        << "\"historical results do not imply future profitability\"]}\n";
}

int run(const Options& options) {
    const auto ticks =
        options.csv_path.empty()
            ? make_synthetic(options.events, options.seed)
            : stockagent::load_market_ticks_csv(
                  options.csv_path);
    const auto candidates = candidate_set(options);
    const auto report =
        stockagent::run_walk_forward(
            ticks, candidates, options.walk_forward);
    if (options.json) {
        print_json(options, ticks, report);
    } else {
        print_human(options, ticks, report);
    }
    return 0;
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        return run(parse_options(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    }
}
