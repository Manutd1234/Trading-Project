#include "stockagent/engine.hpp"
#include "stockagent/risk.hpp"
#include "stockagent/spsc_queue.hpp"
#include "stockagent/strategy.hpp"
#include "stockagent/synthetic_feed.hpp"
#include "stockagent/types.hpp"

#include <atomic>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <thread>

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

void test_queue_fifo_and_capacity() {
    constexpr std::string_view name = "queue_fifo_and_capacity";
    stockagent::SpscQueue<int, 8> queue;
    for (int value = 0; value < 8; ++value) {
        CHECK(name, queue.try_push(value));
    }
    CHECK(name, !queue.try_push(9));
    CHECK(name, queue.approximate_size() == 8U);

    for (int expected = 0; expected < 8; ++expected) {
        int actual = -1;
        CHECK(name, queue.try_pop(actual));
        CHECK(name, actual == expected);
    }
    int ignored = 0;
    CHECK(name, !queue.try_pop(ignored));
}

void test_queue_concurrent_handoff() {
    constexpr std::string_view name = "queue_concurrent_handoff";
    constexpr std::uint64_t count = 100'000;
    stockagent::SpscQueue<std::uint64_t, 1024> queue;
    std::atomic<bool> done{false};

    std::jthread producer([&]() {
        for (std::uint64_t value = 1; value <= count; ++value) {
            while (!queue.try_push(value)) {
                std::this_thread::yield();
            }
        }
        done.store(true, std::memory_order_release);
    });

    std::uint64_t expected = 1;
    std::uint64_t actual = 0;
    while (expected <= count) {
        if (queue.try_pop(actual)) {
            CHECK(name, actual == expected);
            ++expected;
        } else if (!done.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
    CHECK(name, expected == count + 1U);
}

stockagent::MarketTick tick(const std::uint64_t sequence,
                            const stockagent::Quantity bid_quantity = 500,
                            const stockagent::Quantity ask_quantity = 500) {
    return stockagent::MarketTick{
        sequence,
        1'000'000U + (sequence * 1'000U),
        99'995,
        bid_quantity,
        100'005,
        ask_quantity,
    };
}

void test_strategy_warmup_bias_and_exit() {
    constexpr std::string_view name = "strategy_warmup_bias_and_exit";
    stockagent::StrategyConfig config;
    config.fast_window = 2;
    config.slow_window = 3;
    config.momentum_weight = 0.0;
    config.imbalance_weight = 0.0;
    config.external_bias_weight = 1.0;
    config.entry_threshold = 0.5;
    config.exit_threshold = 0.1;
    config.order_quantity = 10;
    config.cooldown_events = 0;

    stockagent::SignalStrategy strategy(config);
    strategy.set_external_bias(2.0);
    CHECK(name, strategy.external_bias() == 1.0);
    strategy.set_external_bias(
        std::numeric_limits<double>::quiet_NaN());
    CHECK(name, strategy.external_bias() == 0.0);
    strategy.set_external_bias(1.0);
    CHECK(name, !strategy.on_tick(tick(1), 0).has_value());
    CHECK(name, !strategy.on_tick(tick(2), 0).has_value());
    const auto entry = strategy.on_tick(tick(3), 0);
    CHECK(name, entry.has_value());
    CHECK(name, entry->side == stockagent::Side::Buy);
    CHECK(name, entry->quantity == 10);

    strategy.set_external_bias(0.0);
    const auto exit = strategy.on_tick(tick(4), 10);
    CHECK(name, exit.has_value());
    CHECK(name, exit->side == stockagent::Side::Sell);
    CHECK(name, exit->quantity == 10);
}

void test_strategy_extreme_quantities_and_config() {
    constexpr std::string_view name =
        "strategy_extreme_quantities_and_config";
    stockagent::StrategyConfig config;
    config.fast_window = 2;
    config.slow_window = 3;
    stockagent::SignalStrategy strategy(config);

    auto extreme = tick(1);
    extreme.bid_quantity = std::numeric_limits<stockagent::Quantity>::max();
    extreme.ask_quantity = std::numeric_limits<stockagent::Quantity>::max();
    CHECK(name, !strategy.on_tick(extreme, 0).has_value());

    config.momentum_weight =
        std::numeric_limits<double>::quiet_NaN();
    bool threw = false;
    try {
        stockagent::SignalStrategy invalid(config);
        static_cast<void>(invalid);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(name, threw);

    config.momentum_weight =
        std::numeric_limits<double>::max();
    threw = false;
    try {
        stockagent::SignalStrategy invalid(config);
        static_cast<void>(invalid);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(name, threw);
}

void test_strategy_ema_crossover() {
    constexpr std::string_view name = "strategy_ema_crossover";
    stockagent::StrategyConfig config;
    config.fast_window = 2;
    config.slow_window = 4;
    config.momentum_weight = 1.0;
    config.imbalance_weight = 0.0;
    config.external_bias_weight = 0.0;
    config.momentum_scale = 1'000.0;
    config.entry_threshold = 0.001;
    config.exit_threshold = 0.0001;
    config.cooldown_events = 0;
    stockagent::SignalStrategy strategy(config);

    std::optional<stockagent::StrategyDecision> decision;
    for (std::uint64_t sequence = 1; sequence <= 6; ++sequence) {
        auto rising = tick(sequence);
        const auto shift =
            static_cast<stockagent::Price>(sequence * 10U);
        rising.bid_ticks += shift;
        rising.ask_ticks += shift;
        decision = strategy.on_tick(rising, 0);
    }
    CHECK(name, decision.has_value());
    if (decision.has_value()) {
        CHECK(name, decision->side == stockagent::Side::Buy);
    }
}

void test_strategy_cooldown_starts_after_acceptance() {
    constexpr std::string_view name =
        "strategy_cooldown_starts_after_acceptance";
    stockagent::StrategyConfig config;
    config.fast_window = 2;
    config.slow_window = 3;
    config.momentum_weight = 0.0;
    config.imbalance_weight = 0.0;
    config.external_bias_weight = 1.0;
    config.entry_threshold = 0.5;
    config.exit_threshold = 0.1;
    config.cooldown_events = 3;
    stockagent::SignalStrategy strategy(config);
    strategy.set_external_bias(1.0);

    static_cast<void>(strategy.on_tick(tick(1), 0));
    static_cast<void>(strategy.on_tick(tick(2), 0));
    CHECK(name, strategy.on_tick(tick(3), 0).has_value());
    CHECK(name, strategy.on_tick(tick(4), 0).has_value());
    strategy.on_order_accepted();
    CHECK(name, !strategy.on_tick(tick(5), 0).has_value());
    CHECK(name, !strategy.on_tick(tick(6), 0).has_value());
    CHECK(name, !strategy.on_tick(tick(7), 0).has_value());
    CHECK(name, strategy.on_tick(tick(8), 0).has_value());
}

void test_strategy_minimum_position_is_safe() {
    constexpr std::string_view name =
        "strategy_minimum_position_is_safe";
    stockagent::StrategyConfig config;
    config.fast_window = 2;
    config.slow_window = 3;
    config.momentum_weight = 0.0;
    config.imbalance_weight = 0.0;
    config.external_bias_weight = 1.0;
    config.entry_threshold = 0.5;
    config.exit_threshold = 0.1;
    config.cooldown_events = 0;
    stockagent::SignalStrategy strategy(config);
    strategy.set_external_bias(1.0);
    static_cast<void>(strategy.on_tick(tick(1), 0));
    static_cast<void>(strategy.on_tick(tick(2), 0));
    const auto decision = strategy.on_tick(
        tick(3), std::numeric_limits<stockagent::Quantity>::min());
    CHECK(name, decision.has_value());
    if (decision.has_value()) {
        CHECK(name, decision->side == stockagent::Side::Buy);
        CHECK(name, decision->quantity == config.order_quantity);
    }
}

void test_risk_event_and_order_limits() {
    constexpr std::string_view name = "risk_event_and_order_limits";
    stockagent::RiskConfig config;
    config.max_order_quantity = 5;
    config.max_absolute_position = 10;
    config.max_gross_notional_ticks = 1'000'000;
    config.max_loss_ticks = 100;
    config.max_event_gap_ns = 5'000;
    stockagent::RiskManager risk(config);
    stockagent::LedgerSnapshot ledger;

    CHECK(name, risk.validate_market(tick(1)) ==
                    stockagent::RiskRejectReason::None);
    CHECK(name, risk.validate_market(tick(1)) ==
                    stockagent::RiskRejectReason::NonMonotonicEvent);

    auto bad_quote = tick(2);
    bad_quote.bid_ticks = bad_quote.ask_ticks + 1;
    CHECK(name, risk.validate_market(bad_quote) ==
                    stockagent::RiskRejectReason::BadQuote);

    auto stale = tick(2);
    stale.timestamp_ns += 10'000;
    CHECK(name, risk.validate_market(stale) ==
                    stockagent::RiskRejectReason::StaleEvent);
    auto recovered = tick(3);
    recovered.timestamp_ns = stale.timestamp_ns + 1'000;
    CHECK(name, risk.validate_market(recovered) ==
                    stockagent::RiskRejectReason::None);

    const stockagent::StrategyDecision too_large{
        stockagent::Side::Buy, 6, 1.0};
    CHECK(name, risk.check_order(too_large, tick(2), ledger) ==
                    stockagent::RiskRejectReason::OrderSize);

    const stockagent::StrategyDecision valid{
        stockagent::Side::Buy, 5, 1.0};
    CHECK(name, risk.check_order(valid, tick(2), ledger) ==
                    stockagent::RiskRejectReason::None);

    auto shallow = tick(2);
    shallow.ask_quantity = 4;
    CHECK(name, risk.check_order(valid, shallow, ledger) ==
                    stockagent::RiskRejectReason::InsufficientLiquidity);

    ledger.position = 10;
    CHECK(name, risk.check_order(valid, tick(2), ledger) ==
                    stockagent::RiskRejectReason::PositionLimit);

    ledger.position = 0;
    ledger.total_pnl_ticks = -100;
    CHECK(name, risk.check_order(valid, tick(2), ledger) ==
                    stockagent::RiskRejectReason::LossLimit);

    ledger.position = 5;
    const stockagent::StrategyDecision flatten{
        stockagent::Side::Sell, 5, -1.0};
    CHECK(name, risk.check_order(flatten, tick(2), ledger) ==
                    stockagent::RiskRejectReason::None);

    risk.set_kill_switch(true);
    CHECK(name, risk.check_order(valid, tick(2), ledger) ==
                    stockagent::RiskRejectReason::KillSwitch);
    CHECK(name, risk.check_order(flatten, tick(2), ledger) ==
                    stockagent::RiskRejectReason::None);

    auto repriced = tick(2);
    repriced.bid_ticks = 300'000;
    repriced.ask_ticks = 300'010;
    CHECK(name, risk.check_order(flatten, repriced, ledger) ==
                    stockagent::RiskRejectReason::None);
}

stockagent::EngineConfig deterministic_engine_config() {
    stockagent::EngineConfig config;
    config.strategy.fast_window = 2;
    config.strategy.slow_window = 3;
    config.strategy.momentum_weight = 0.0;
    config.strategy.imbalance_weight = 1.0;
    config.strategy.external_bias_weight = 0.0;
    config.strategy.entry_threshold = 0.5;
    config.strategy.exit_threshold = 0.1;
    config.strategy.order_quantity = 10;
    config.strategy.cooldown_events = 0;
    return config;
}

void test_engine_fill_and_pnl() {
    constexpr std::string_view name = "engine_fill_and_pnl";
    stockagent::TradingEngine engine(deterministic_engine_config());
    CHECK(name, !engine.on_market_tick(tick(1, 900, 100)).has_value());
    CHECK(name, !engine.on_market_tick(tick(2, 900, 100)).has_value());

    const auto buy = engine.on_market_tick(tick(3, 900, 100));
    CHECK(name, buy.has_value());
    CHECK(name, buy->side == stockagent::Side::Buy);
    CHECK(name, buy->price_ticks == 100'005);
    CHECK(name, engine.ledger().position == 10);
    CHECK(name,
          engine.ledger().realized_pnl_ticks +
                  engine.ledger().unrealized_pnl_ticks ==
              engine.ledger().total_pnl_ticks);

    const auto sell = engine.on_market_tick(tick(4, 100, 900));
    CHECK(name, sell.has_value());
    CHECK(name, sell->side == stockagent::Side::Sell);
    CHECK(name, sell->price_ticks == 99'995);
    CHECK(name, engine.ledger().position == 0);
    CHECK(name, engine.ledger().realized_pnl_ticks == -100);
    CHECK(name, engine.ledger().total_pnl_ticks == -100);
    CHECK(name,
          engine.ledger().realized_pnl_ticks +
                  engine.ledger().unrealized_pnl_ticks ==
              engine.ledger().total_pnl_ticks);
    CHECK(name, engine.stats().fills == 2U);

    auto invalid = tick(5);
    invalid.ask_quantity = 0;
    CHECK(name, !engine.on_market_tick(invalid).has_value());
    CHECK(name,
          engine.stats().rejection_counts[static_cast<std::size_t>(
              stockagent::RiskRejectReason::BadQuote)] == 1U);
    CHECK(name, engine.stats().events_rejected == 1U);
    CHECK(name, engine.stats().orders_rejected == 0U);
}

void test_engine_liquidity_rejection() {
    constexpr std::string_view name = "engine_liquidity_rejection";
    auto config = deterministic_engine_config();
    stockagent::TradingEngine engine(config);
    CHECK(name, !engine.on_market_tick(tick(1, 900, 5)).has_value());
    CHECK(name, !engine.on_market_tick(tick(2, 900, 5)).has_value());
    CHECK(name, !engine.on_market_tick(tick(3, 900, 5)).has_value());
    CHECK(name, engine.stats().strategy_signals == 1U);
    CHECK(name, engine.stats().orders_rejected == 1U);
    CHECK(name,
          engine.stats().rejection_counts[static_cast<std::size_t>(
              stockagent::RiskRejectReason::InsufficientLiquidity)] == 1U);
}

void test_engine_shallow_book_reduces_position() {
    constexpr std::string_view name =
        "engine_shallow_book_reduces_position";
    stockagent::TradingEngine engine(deterministic_engine_config());
    static_cast<void>(engine.on_market_tick(tick(1, 900, 100)));
    static_cast<void>(engine.on_market_tick(tick(2, 900, 100)));
    CHECK(name,
          engine.on_market_tick(tick(3, 900, 100)).has_value());
    CHECK(name, engine.ledger().position == 10);

    const auto first_close =
        engine.on_market_tick(tick(4, 5, 900));
    CHECK(name, first_close.has_value());
    if (first_close.has_value()) {
        CHECK(name, first_close->side == stockagent::Side::Sell);
        CHECK(name, first_close->quantity == 5);
    }
    CHECK(name, engine.ledger().position == 5);

    const auto second_close =
        engine.on_market_tick(tick(5, 5, 900));
    CHECK(name, second_close.has_value());
    if (second_close.has_value()) {
        CHECK(name, second_close->quantity == 5);
    }
    CHECK(name, engine.ledger().position == 0);
}

void test_engine_arithmetic_fault_is_fail_closed() {
    constexpr std::string_view name =
        "engine_arithmetic_fault_is_fail_closed";
    stockagent::TradingEngine engine(deterministic_engine_config());
    static_cast<void>(engine.on_market_tick(tick(1, 900, 100)));
    static_cast<void>(engine.on_market_tick(tick(2, 900, 100)));
    CHECK(name,
          engine.on_market_tick(tick(3, 900, 100)).has_value());
    CHECK(name, engine.ledger().position == 10);
    const auto previous_total = engine.ledger().total_pnl_ticks;

    auto extreme = tick(4);
    extreme.bid_ticks =
        std::numeric_limits<stockagent::Price>::max() - 10;
    extreme.ask_ticks =
        std::numeric_limits<stockagent::Price>::max();
    CHECK(name, !engine.on_market_tick(extreme).has_value());
    CHECK(name, engine.faulted());
    CHECK(name, engine.ledger().position == 10);
    CHECK(name, engine.ledger().total_pnl_ticks == previous_total);
    CHECK(name,
          engine.stats().rejection_counts[static_cast<std::size_t>(
              stockagent::RiskRejectReason::ArithmeticOverflow)] == 1U);

    CHECK(name, !engine.on_market_tick(tick(5, 100, 900)).has_value());
    CHECK(name,
          engine.stats().rejection_counts[static_cast<std::size_t>(
              stockagent::RiskRejectReason::ArithmeticOverflow)] == 2U);
}

void test_deterministic_replay() {
    constexpr std::string_view name = "deterministic_replay";
    stockagent::TradingEngine first;
    stockagent::TradingEngine second;
    stockagent::SyntheticFeed first_feed(123);
    stockagent::SyntheticFeed second_feed(123);

    for (std::size_t index = 0; index < 5'000; ++index) {
        static_cast<void>(first.on_market_tick(first_feed.next()));
        static_cast<void>(second.on_market_tick(second_feed.next()));
    }

    CHECK(name, first.ledger().position == second.ledger().position);
    CHECK(name, first.ledger().cash_flow_ticks ==
                    second.ledger().cash_flow_ticks);
    CHECK(name, first.ledger().realized_pnl_ticks ==
                    second.ledger().realized_pnl_ticks);
    CHECK(name, first.ledger().total_pnl_ticks ==
                    second.ledger().total_pnl_ticks);
    CHECK(name, first.stats().fills == second.stats().fills);
    CHECK(name, first.stats().orders_rejected ==
                    second.stats().orders_rejected);
}

void test_concurrent_control_updates() {
    constexpr std::string_view name = "concurrent_control_updates";
    stockagent::TradingEngine engine;
    stockagent::SyntheticFeed feed(987);
    std::atomic<bool> finished{false};

    std::jthread controls([&]() {
        std::uint64_t iteration = 0;
        while (!finished.load(std::memory_order_acquire)) {
            engine.set_external_bias(
                (iteration & 1U) == 0U ? 0.25 : -0.25);
            engine.set_kill_switch((iteration % 17U) == 0U);
            ++iteration;
        }
        engine.set_kill_switch(false);
        engine.set_external_bias(0.0);
    });

    for (std::size_t index = 0; index < 50'000; ++index) {
        static_cast<void>(engine.on_market_tick(feed.next()));
    }
    finished.store(true, std::memory_order_release);
    controls.join();
    CHECK(name, !engine.faulted());
    CHECK(name, engine.stats().events_received == 50'000U);
}

}  // namespace

int main() {
    try {
        test_queue_fifo_and_capacity();
        test_queue_concurrent_handoff();
        test_strategy_warmup_bias_and_exit();
        test_strategy_extreme_quantities_and_config();
        test_strategy_ema_crossover();
        test_strategy_cooldown_starts_after_acceptance();
        test_strategy_minimum_position_is_safe();
        test_risk_event_and_order_limits();
        test_engine_fill_and_pnl();
        test_engine_liquidity_rejection();
        test_engine_shallow_book_reduces_position();
        test_engine_arithmetic_fault_is_fail_closed();
        test_deterministic_replay();
        test_concurrent_control_updates();
    } catch (const std::exception& error) {
        std::cerr << "UNEXPECTED EXCEPTION: " << error.what() << '\n';
        return 2;
    }

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "All StockAgent C++ tests passed\n";
    return 0;
}
