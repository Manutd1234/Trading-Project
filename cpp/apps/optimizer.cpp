#include "stockagent/csv_replay.hpp"
#include "stockagent/engine.hpp"
#include "stockagent/strategy.hpp"
#include "stockagent/synthetic_feed.hpp"
#include "stockagent/types.hpp"

#include <algorithm>
#include <array>
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
    std::size_t events{50'000};
    std::uint64_t seed{42};
    std::size_t train_percent{70};
    std::int64_t fee_ticks_per_fill{10};
    std::string csv_path;
};

struct Metrics {
    std::int64_t pnl_ticks{};
    long double net_liquidation_pnl_ticks{};
    std::uint64_t fills{};
    long double max_drawdown_ticks{};
    long double objective{};
    bool faulted{false};
};

struct CandidateResult {
    stockagent::StrategyConfig config{};
    Metrics metrics{};
};

[[nodiscard]] std::uint64_t parse_unsigned(
    const std::string_view text, const std::string_view name) {
    std::uint64_t value{};
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        throw std::invalid_argument("invalid value for " + std::string(name));
    }
    return value;
}

[[nodiscard]] Options parse_options(const int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg(argv[index]);
        const auto value = [&]() -> std::string_view {
            if ((index + 1) >= argc) {
                throw std::invalid_argument("missing value for " +
                                            std::string(arg));
            }
            ++index;
            return argv[index];
        };

        if (arg == "--events") {
            const auto parsed = parse_unsigned(value(), arg);
            if (parsed < 500U || parsed > 2'000'000U) {
                throw std::invalid_argument(
                    "--events must be between 500 and 2000000");
            }
            options.events = static_cast<std::size_t>(parsed);
        } else if (arg == "--seed") {
            options.seed = parse_unsigned(value(), arg);
        } else if (arg == "--train-percent") {
            const auto parsed = parse_unsigned(value(), arg);
            if (parsed < 50U || parsed > 90U) {
                throw std::invalid_argument(
                    "--train-percent must be between 50 and 90");
            }
            options.train_percent = static_cast<std::size_t>(parsed);
        } else if (arg == "--fee-ticks-per-fill") {
            const auto parsed = parse_unsigned(value(), arg);
            if (parsed >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())) {
                throw std::invalid_argument(
                    "--fee-ticks-per-fill is too large");
            }
            options.fee_ticks_per_fill =
                static_cast<std::int64_t>(parsed);
        } else if (arg == "--csv") {
            options.csv_path = value();
        } else if (arg == "--help" || arg == "-h") {
            std::cout
                << "StockAgent train/holdout strategy optimizer\n\n"
                << "Usage: stockagent_strategy_optimizer "
                   "[--events N] [--seed N] [--csv PATH] "
                   "[--train-percent N] [--fee-ticks-per-fill N]\n";
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown option: " +
                                        std::string(arg));
        }
    }
    return options;
}

[[nodiscard]] std::vector<stockagent::MarketTick> make_synthetic(
    const std::size_t count, const std::uint64_t seed) {
    std::vector<stockagent::MarketTick> ticks;
    ticks.reserve(count);
    stockagent::SyntheticFeed feed(seed);
    for (std::size_t index = 0; index < count; ++index) {
        ticks.push_back(feed.next());
    }
    return ticks;
}

[[nodiscard]] Metrics evaluate(
    const stockagent::StrategyConfig& strategy,
    const std::vector<stockagent::MarketTick>& ticks,
    const std::size_t begin, const std::size_t end,
    const std::int64_t fee_ticks_per_fill) {
    stockagent::EngineConfig config;
    config.strategy = strategy;
    config.risk.max_loss_ticks = 0;
    config.risk.max_event_gap_ns = 0;
    stockagent::TradingEngine engine(config);

    if (begin > 0) {
        const auto warmup_events =
            std::min(begin, strategy.slow_window - 1U);
        engine.set_kill_switch(true);
        for (std::size_t index = begin - warmup_events;
             index < begin; ++index) {
            static_cast<void>(engine.on_market_tick(ticks[index]));
        }
        engine.set_kill_switch(false);
    }

    long double equity_peak{};
    long double max_drawdown{};
    long double net_liquidation_equity{};
    for (std::size_t index = begin; index < end; ++index) {
        static_cast<void>(engine.on_market_tick(ticks[index]));
        const auto& ledger = engine.ledger();
        const long double gross_equity =
            static_cast<long double>(engine.ledger().total_pnl_ticks);
        long double liquidation_adjustment{};
        if (ledger.position != 0) {
            const auto exit_price =
                ledger.position > 0 ? ticks[index].bid_ticks
                                    : ticks[index].ask_ticks;
            liquidation_adjustment =
                static_cast<long double>(
                    exit_price - ledger.mark_price_ticks) *
                static_cast<long double>(ledger.position);
        }
        const auto charged_fills =
            engine.stats().fills + (ledger.position == 0 ? 0U : 1U);
        const long double fees =
            static_cast<long double>(fee_ticks_per_fill) *
            static_cast<long double>(charged_fills);
        net_liquidation_equity =
            gross_equity + liquidation_adjustment - fees;
        equity_peak = std::max(equity_peak, net_liquidation_equity);
        max_drawdown = std::max(
            max_drawdown, equity_peak - net_liquidation_equity);
        if (engine.faulted()) {
            break;
        }
    }

    const auto pnl = engine.ledger().total_pnl_ticks;
    const auto fills = engine.stats().fills;
    return Metrics{
        pnl,
        net_liquidation_equity,
        fills,
        max_drawdown,
        net_liquidation_equity - max_drawdown,
        engine.faulted(),
    };
}

[[nodiscard]] CandidateResult optimize(
    const std::vector<stockagent::MarketTick>& ticks,
    const std::size_t train_end,
    const std::int64_t fee_ticks_per_fill,
    std::size_t& candidates_evaluated) {
    constexpr std::array<std::size_t, 3> fast_windows{4, 8, 12};
    constexpr std::array<std::size_t, 3> slow_windows{24, 32, 48};
    constexpr std::array<double, 3> entry_thresholds{0.20, 0.30, 0.40};
    constexpr std::array<double, 3> momentum_weights{0.25, 0.50, 0.75};
    constexpr std::array<std::uint64_t, 2> cooldowns{2, 6};

    CandidateResult best;
    best.metrics.objective =
        -std::numeric_limits<long double>::infinity();

    for (const auto fast : fast_windows) {
        for (const auto slow : slow_windows) {
            for (const auto entry : entry_thresholds) {
                for (const auto momentum_weight : momentum_weights) {
                    for (const auto cooldown : cooldowns) {
                        stockagent::StrategyConfig config;
                        config.fast_window = fast;
                        config.slow_window = slow;
                        config.entry_threshold = entry;
                        config.exit_threshold = entry / 4.0;
                        config.momentum_weight = momentum_weight;
                        config.imbalance_weight =
                            1.0 - momentum_weight;
                        config.external_bias_weight = 0.0;
                        config.cooldown_events = cooldown;

                        const auto metrics =
                            evaluate(config, ticks, 0, train_end,
                                     fee_ticks_per_fill);
                        ++candidates_evaluated;
                        const bool better =
                            !metrics.faulted &&
                            (metrics.objective >
                                 best.metrics.objective ||
                             (metrics.objective ==
                                  best.metrics.objective &&
                              metrics.fills < best.metrics.fills));
                        if (better) {
                            best = CandidateResult{config, metrics};
                        }
                    }
                }
            }
        }
    }
    return best;
}

void print_metrics(const std::string_view label,
                   const Metrics& metrics) {
    std::cout << label << "_pnl_ticks: " << metrics.pnl_ticks << '\n'
              << label << "_net_liquidation_pnl_ticks: "
              << static_cast<double>(
                     metrics.net_liquidation_pnl_ticks) << '\n'
              << label << "_fills: " << metrics.fills << '\n'
              << label << "_max_drawdown_ticks: "
              << static_cast<double>(metrics.max_drawdown_ticks) << '\n'
              << label << "_objective: "
              << static_cast<double>(metrics.objective) << '\n'
              << label << "_faulted: "
              << (metrics.faulted ? "true" : "false") << '\n';
}

int run(const Options& options) {
    const auto ticks =
        options.csv_path.empty()
            ? make_synthetic(options.events, options.seed)
            : stockagent::load_market_ticks_csv(options.csv_path);
    if (ticks.size() < 500U) {
        throw std::runtime_error(
            "optimizer requires at least 500 ordered market events");
    }

    const std::size_t train_end =
        (ticks.size() * options.train_percent) / 100U;
    if ((ticks.size() - train_end) < 50U) {
        throw std::runtime_error("validation segment is too small");
    }

    std::size_t candidates_evaluated = 0;
    const auto best =
        optimize(ticks, train_end, options.fee_ticks_per_fill,
                 candidates_evaluated);
    if (!std::isfinite(best.metrics.objective)) {
        throw std::runtime_error("all strategy candidates faulted");
    }
    const auto validation =
        evaluate(best.config, ticks, train_end, ticks.size(),
                 options.fee_ticks_per_fill);

    std::cout << std::fixed << std::setprecision(2)
              << "StockAgent train/holdout parameter search\n"
              << "source: "
              << (options.csv_path.empty() ? "synthetic" : options.csv_path)
              << '\n'
              << "events: " << ticks.size() << '\n'
              << "train_events: " << train_end << '\n'
              << "validation_events: " << (ticks.size() - train_end) << '\n'
              << "candidates_evaluated: " << candidates_evaluated << '\n'
              << "fee_ticks_per_fill: " << options.fee_ticks_per_fill
              << "\n\nbest_parameters\n"
              << "fast_window: " << best.config.fast_window << '\n'
              << "slow_window: " << best.config.slow_window << '\n'
              << "entry_threshold: " << best.config.entry_threshold << '\n'
              << "exit_threshold: " << best.config.exit_threshold << '\n'
              << "momentum_weight: " << best.config.momentum_weight << '\n'
              << "imbalance_weight: " << best.config.imbalance_weight << '\n'
              << "cooldown_events: " << best.config.cooldown_events
              << "\n\n";
    print_metrics("train", best.metrics);
    std::cout << '\n';
    print_metrics("validation", validation);
    std::cout
        << "\nThe selected parameters maximize only the stated in-sample "
           "objective. Validation performance is reported separately and "
           "does not imply future profitability.\n";
    return validation.faulted ? 3 : 0;
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
