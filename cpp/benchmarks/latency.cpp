#include "stockagent/engine.hpp"
#include "stockagent/synthetic_feed.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
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
#include <vector>

namespace {

struct Options {
    std::size_t events{500'000};
    std::size_t warmup{20'000};
    std::size_t sample_every{1};
    std::uint64_t seed{42};
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
            if (parsed == 0 || parsed > 5'000'000U) {
                throw std::invalid_argument(
                    "--events must be between 1 and 5000000");
            }
            options.events = static_cast<std::size_t>(parsed);
        } else if (arg == "--warmup") {
            options.warmup = static_cast<std::size_t>(
                parse_unsigned(value(), arg));
        } else if (arg == "--sample-every") {
            const auto parsed = parse_unsigned(value(), arg);
            if (parsed == 0) {
                throw std::invalid_argument(
                    "--sample-every must be at least 1");
            }
            options.sample_every = static_cast<std::size_t>(parsed);
        } else if (arg == "--seed") {
            options.seed = parse_unsigned(value(), arg);
        } else if (arg == "--help" || arg == "-h") {
            std::cout
                << "Usage: stockagent_latency_bench "
                   "[--events N] [--warmup N] [--sample-every N] "
                   "[--seed N]\n";
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown option: " + std::string(arg));
        }
    }
    return options;
}

[[nodiscard]] std::uint64_t percentile(
    const std::vector<std::uint64_t>& sorted, const double percent) {
    const double rank =
        std::ceil((percent / 100.0) * static_cast<double>(sorted.size()));
    const auto index = static_cast<std::size_t>(std::max(1.0, rank)) - 1U;
    return sorted[std::min(index, sorted.size() - 1U)];
}

[[nodiscard]] double average_latency(
    const std::vector<std::uint64_t>& values) {
    long double sum{};
    for (const auto value : values) {
        sum += static_cast<long double>(value);
    }
    return static_cast<double>(
        sum / static_cast<long double>(values.size()));
}

void print_distribution(
    const std::string_view label,
    const std::vector<std::uint64_t>& sorted) {
    std::cout << "  " << label << "_samples: " << sorted.size() << '\n';
    if (sorted.empty()) {
        return;
    }
    std::cout << "  " << label << "_average_ns: "
              << average_latency(sorted) << '\n'
              << "  " << label << "_p50_ns: "
              << percentile(sorted, 50.0) << '\n'
              << "  " << label << "_p95_ns: "
              << percentile(sorted, 95.0) << '\n'
              << "  " << label << "_p99_ns: "
              << percentile(sorted, 99.0) << '\n'
              << "  " << label << "_p99_9_ns: "
              << percentile(sorted, 99.9) << '\n'
              << "  " << label << "_max_ns: " << sorted.back() << '\n';
}

int run(const Options& options) {
    using Clock = std::chrono::steady_clock;

    stockagent::TradingEngine throughput_engine;
    stockagent::SyntheticFeed throughput_feed(options.seed);
    for (std::size_t index = 0; index < options.warmup; ++index) {
        static_cast<void>(
            throughput_engine.on_market_tick(throughput_feed.next()));
    }
    const auto warmup_fills = throughput_engine.stats().fills;

    stockagent::SyntheticFeed data_feed(options.seed);
    for (std::size_t index = 0; index < options.warmup; ++index) {
        static_cast<void>(data_feed.next());
    }
    std::vector<stockagent::MarketTick> measured_ticks;
    measured_ticks.reserve(options.events);
    for (std::size_t index = 0; index < options.events; ++index) {
        measured_ticks.push_back(data_feed.next());
    }

    const auto throughput_start = Clock::now();
    for (const auto& tick : measured_ticks) {
        static_cast<void>(throughput_engine.on_market_tick(tick));
    }
    const auto throughput_end = Clock::now();
    const double throughput_seconds =
        std::chrono::duration<double>(throughput_end - throughput_start)
            .count();
    const double throughput =
        static_cast<double>(options.events) / throughput_seconds;
    const auto measured_fills =
        throughput_engine.stats().fills - warmup_fills;

    std::vector<std::uint64_t> timer_samples;
    timer_samples.reserve(10'000);
    for (std::size_t index = 0; index < 10'000; ++index) {
        const auto start = Clock::now();
        const auto end = Clock::now();
        timer_samples.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
                .count()));
    }
    std::sort(timer_samples.begin(), timer_samples.end());
    const auto timer_baseline_median_ns =
        percentile(timer_samples, 50.0);

    stockagent::TradingEngine latency_engine;
    stockagent::SyntheticFeed latency_warmup_feed(options.seed);
    for (std::size_t index = 0; index < options.warmup; ++index) {
        static_cast<void>(
            latency_engine.on_market_tick(latency_warmup_feed.next()));
    }

    std::vector<std::uint64_t> latency_ns;
    std::vector<std::uint64_t> fill_latency_ns;
    std::vector<std::uint64_t> no_fill_latency_ns;
    latency_ns.reserve(
        (options.events / options.sample_every) + 1U);
    fill_latency_ns.reserve(
        (options.events / options.sample_every / 100U) + 1U);
    no_fill_latency_ns.reserve(
        (options.events / options.sample_every) + 1U);
    for (std::size_t index = 0; index < options.events; ++index) {
        const auto& tick = measured_ticks[index];
        if ((index % options.sample_every) != 0U) {
            static_cast<void>(latency_engine.on_market_tick(tick));
            continue;
        }
        const auto start = Clock::now();
        const auto fill = latency_engine.on_market_tick(tick);
        const auto end = Clock::now();
        const auto measured = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
                .count());
        latency_ns.push_back(measured);
        if (fill.has_value()) {
            fill_latency_ns.push_back(measured);
        } else {
            no_fill_latency_ns.push_back(measured);
        }
    }
    std::sort(latency_ns.begin(), latency_ns.end());
    std::sort(fill_latency_ns.begin(), fill_latency_ns.end());
    std::sort(no_fill_latency_ns.begin(), no_fill_latency_ns.end());

    std::cout << "StockAgent engine latency benchmark\n"
              << "  build: use Release for meaningful comparisons\n"
              << "  warmup_events: " << options.warmup << '\n'
              << "  measured_events: " << options.events << '\n'
              << "  latency_sample_every: " << options.sample_every << '\n'
              << "  latency_samples: " << latency_ns.size() << '\n'
              << "  timer_baseline_median_ns: "
              << timer_baseline_median_ns << '\n'
              << std::fixed << std::setprecision(6)
              << "  throughput_elapsed_seconds: " << throughput_seconds
              << '\n' << std::setprecision(2)
              << "  throughput_events_per_second: " << throughput << '\n'
              << "  throughput_fills: " << measured_fills << '\n';
    print_distribution("all", latency_ns);
    print_distribution("fill", fill_latency_ns);
    print_distribution("no_fill", no_fill_latency_ns);
    std::cout
              << "  final_position: "
              << throughput_engine.ledger().position << '\n'
              << "  total_pnl_ticks: "
              << throughput_engine.ledger().total_pnl_ticks << '\n'
              << "\nThroughput is measured without per-event timing. "
                 "Latency is raw steady-clock wall time and includes timer "
                 "overhead/quantization; compare only on the same isolated "
                 "host and build.\n";
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
