#include "stockagent/csv_replay.hpp"
#include "stockagent/engine.hpp"
#include "stockagent/spsc_queue.hpp"
#include "stockagent/synthetic_feed.hpp"
#include "stockagent/types.hpp"

#include <atomic>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using stockagent::MarketTick;

struct Options {
    std::size_t events{10'000};
    std::uint64_t seed{42};
    std::string csv_path;
    double external_bias{};
    bool quiet{false};
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

[[nodiscard]] double parse_double(const std::string_view text,
                                  const std::string_view name) {
    std::string owned(text);
    char* end = nullptr;
    const double value = std::strtod(owned.c_str(), &end);
    if (end != owned.c_str() + owned.size() || !std::isfinite(value)) {
        throw std::invalid_argument("invalid value for " + std::string(name));
    }
    return value;
}

void print_help() {
    std::cout
        << "StockAgent low-latency paper trader\n\n"
        << "Usage:\n"
        << "  stockagent_trader [--events N] [--seed N] [--quiet]\n"
        << "  stockagent_trader --csv PATH [--external-bias VALUE] "
           "[--quiet]\n\n"
        << "Options:\n"
        << "  --events N           Number of synthetic events (default 10000)\n"
        << "  --seed N             Deterministic synthetic seed (default 42)\n"
        << "  --csv PATH           Replay CSV market data instead\n"
        << "  --external-bias X    Slow signal in [-1, 1] (default 0)\n"
        << "  --quiet              Suppress per-fill output\n"
        << "  --help                Show this message\n";
}

[[nodiscard]] Options parse_options(const int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg(argv[index]);
        const auto require_value = [&]() -> std::string_view {
            if ((index + 1) >= argc) {
                throw std::invalid_argument("missing value for " +
                                            std::string(arg));
            }
            ++index;
            return argv[index];
        };

        if (arg == "--events") {
            const auto value = parse_unsigned(require_value(), arg);
            if (value == 0 || value > 10'000'000U) {
                throw std::invalid_argument(
                    "--events must be between 1 and 10000000");
            }
            options.events = static_cast<std::size_t>(value);
        } else if (arg == "--seed") {
            options.seed = parse_unsigned(require_value(), arg);
        } else if (arg == "--csv") {
            options.csv_path = require_value();
        } else if (arg == "--external-bias") {
            options.external_bias = parse_double(require_value(), arg);
            if (options.external_bias < -1.0 ||
                options.external_bias > 1.0) {
                throw std::invalid_argument(
                    "--external-bias must be in [-1, 1]");
            }
        } else if (arg == "--quiet") {
            options.quiet = true;
        } else if (arg == "--help" || arg == "-h") {
            print_help();
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown option: " + std::string(arg));
        }
    }
    return options;
}

[[nodiscard]] std::vector<MarketTick> make_synthetic(
    const std::size_t count, const std::uint64_t seed) {
    std::vector<MarketTick> ticks;
    ticks.reserve(count);
    stockagent::SyntheticFeed feed(seed);
    for (std::size_t index = 0; index < count; ++index) {
        ticks.push_back(feed.next());
    }
    return ticks;
}

void print_fill(const stockagent::Fill& fill) {
    std::cout << "fill order=" << fill.order_id
              << " seq=" << fill.market_sequence
              << " side=" << stockagent::to_string(fill.side)
              << " quantity=" << fill.quantity
              << " price_ticks=" << fill.price_ticks
              << " score=" << std::fixed << std::setprecision(4)
              << fill.signal_score << '\n';
}

void print_report(const stockagent::TradingEngine& engine) {
    const auto& stats = engine.stats();
    const auto& ledger = engine.ledger();
    std::cout << "\nrun_summary\n"
              << "  events_received: " << stats.events_received << '\n'
              << "  events_accepted: " << stats.events_accepted << '\n'
              << "  events_rejected: " << stats.events_rejected << '\n'
              << "  strategy_signals: " << stats.strategy_signals << '\n'
              << "  orders_accepted: " << stats.orders_accepted << '\n'
              << "  orders_rejected: " << stats.orders_rejected << '\n'
              << "  fills: " << stats.fills << '\n'
              << "  arithmetic_faulted: "
              << (engine.faulted() ? "true" : "false") << '\n'
              << "  final_position: " << ledger.position << '\n'
              << "  average_entry_price_ticks: "
              << ledger.average_entry_price_ticks << '\n'
              << "  realized_pnl_ticks: " << ledger.realized_pnl_ticks << '\n'
              << "  unrealized_pnl_ticks: " << ledger.unrealized_pnl_ticks
              << '\n'
              << "  total_pnl_ticks: " << ledger.total_pnl_ticks << '\n';

    for (std::size_t index = 1;
         index < stockagent::risk_reject_reason_count; ++index) {
        if (stats.rejection_counts[index] == 0) {
            continue;
        }
        const auto reason =
            static_cast<stockagent::RiskRejectReason>(index);
        std::cout << "  rejected_" << stockagent::to_string(reason) << ": "
                  << stats.rejection_counts[index] << '\n';
    }
}

int run(const Options& options) {
    const auto ticks = options.csv_path.empty()
                           ? make_synthetic(options.events, options.seed)
                           : stockagent::load_market_ticks_csv(
                                 options.csv_path);

    stockagent::TradingEngine engine;
    engine.set_external_bias(options.external_bias);

    stockagent::SpscQueue<MarketTick, 4096> queue;
    std::atomic<bool> producer_done{false};
    std::jthread producer([&]() {
        for (const auto& tick : ticks) {
            while (!queue.try_push(tick)) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    MarketTick tick;
    const auto process_tick = [&](const MarketTick& market_tick) {
        const auto fill = engine.on_market_tick(market_tick);
        if (fill.has_value() && !options.quiet) {
            print_fill(*fill);
        }
    };
    while (true) {
        if (queue.try_pop(tick)) {
            process_tick(tick);
            continue;
        }
        if (producer_done.load(std::memory_order_acquire)) {
            // Re-read the queue after acquiring producer completion. This
            // pairs with the producer's release and prevents a stale empty
            // observation from dropping the final published events.
            while (queue.try_pop(tick)) {
                process_tick(tick);
            }
            break;
        }
        std::this_thread::yield();
    }

    if (engine.stats().events_received != ticks.size()) {
        throw std::runtime_error(
            "SPSC drain invariant failed: not all feed events were processed");
    }
    print_report(engine);
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
