#pragma once

#include "stockagent/types.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>

namespace stockagent {

struct StrategyConfig {
    std::size_t fast_window{8};
    std::size_t slow_window{32};
    double momentum_weight{0.45};
    double imbalance_weight{0.45};
    double external_bias_weight{0.10};
    double momentum_scale{120.0};
    double entry_threshold{0.30};
    double exit_threshold{0.08};
    Quantity order_quantity{10};
    std::uint64_t cooldown_events{4};
};

class SignalStrategy {
  public:
    explicit SignalStrategy(StrategyConfig config) : config_(config) {
        if (config_.fast_window == 0 ||
            config_.slow_window <= config_.fast_window) {
            throw std::invalid_argument(
                "slow_window must be greater than fast_window");
        }
        if (config_.order_quantity <= 0) {
            throw std::invalid_argument("order_quantity must be positive");
        }
        if (config_.entry_threshold <= 0.0 ||
            config_.exit_threshold < 0.0 ||
            config_.exit_threshold >= config_.entry_threshold) {
            throw std::invalid_argument(
                "thresholds must satisfy 0 <= exit < entry");
        }
        const double weight_sum =
            config_.momentum_weight + config_.imbalance_weight +
            config_.external_bias_weight;
        if (!std::isfinite(config_.momentum_weight) ||
            !std::isfinite(config_.imbalance_weight) ||
            !std::isfinite(config_.external_bias_weight) ||
            !std::isfinite(config_.momentum_scale) ||
            !std::isfinite(config_.entry_threshold) ||
            !std::isfinite(config_.exit_threshold) ||
            !std::isfinite(weight_sum) ||
            config_.momentum_weight < 0.0 ||
            config_.imbalance_weight < 0.0 ||
            config_.external_bias_weight < 0.0 ||
            config_.momentum_weight > 1.0 ||
            config_.imbalance_weight > 1.0 ||
            config_.external_bias_weight > 1.0 ||
            config_.momentum_scale <= 0.0 ||
            config_.momentum_scale > 1'000'000.0 ||
            config_.entry_threshold > 1.0 ||
            weight_sum <= 0.0 || weight_sum > 1.0) {
            throw std::invalid_argument(
                "strategy weights, scale, and thresholds are out of range");
        }
    }

    void set_external_bias(const double value) noexcept {
        const double sanitized =
            std::isfinite(value) ? std::clamp(value, -1.0, 1.0) : 0.0;
        external_bias_scaled_.store(
            static_cast<std::int32_t>(
                std::lround(sanitized * external_bias_scale)),
            std::memory_order_relaxed);
    }

    [[nodiscard]] double external_bias() const noexcept {
        return static_cast<double>(
                   external_bias_scaled_.load(std::memory_order_relaxed)) /
               external_bias_scale;
    }

    [[nodiscard]] double last_score() const noexcept { return last_score_; }

    void on_order_accepted() noexcept {
        has_ordered_ = true;
        last_order_event_ = event_count_;
    }

    [[nodiscard]] std::optional<StrategyDecision> on_tick(
        const MarketTick& tick, const Quantity position) noexcept {
        ++event_count_;
        const auto mid_ticks =
            tick.bid_ticks + ((tick.ask_ticks - tick.bid_ticks) / 2);
        const double mid = static_cast<double>(mid_ticks);

        if (!initialized_) {
            fast_ema_ = mid;
            slow_ema_ = mid;
            initialized_ = true;
        } else {
            const double fast_alpha =
                2.0 / (static_cast<double>(config_.fast_window) + 1.0);
            const double slow_alpha =
                2.0 / (static_cast<double>(config_.slow_window) + 1.0);
            fast_ema_ += fast_alpha * (mid - fast_ema_);
            slow_ema_ += slow_alpha * (mid - slow_ema_);
        }

        const double momentum =
            std::clamp(((fast_ema_ - slow_ema_) / std::max(1.0, mid)) *
                           config_.momentum_scale,
                       -1.0, 1.0);
        const double total_size =
            static_cast<double>(tick.bid_quantity) +
            static_cast<double>(tick.ask_quantity);
        const double imbalance =
            total_size > 0.0
                ? (static_cast<double>(tick.bid_quantity) -
                   static_cast<double>(tick.ask_quantity)) /
                      total_size
                : 0.0;
        last_score_ =
            (config_.momentum_weight * momentum) +
            (config_.imbalance_weight * imbalance) +
            (config_.external_bias_weight * external_bias());

        if (event_count_ < config_.slow_window) {
            return std::nullopt;
        }
        if (has_ordered_ &&
            (event_count_ - last_order_event_) <= config_.cooldown_events) {
            return std::nullopt;
        }

        std::optional<StrategyDecision> decision;
        if (last_score_ >= config_.entry_threshold) {
            if (position < 0) {
                const auto quantity = close_quantity(
                    position, tick.ask_quantity);
                if (quantity > 0) {
                    decision = StrategyDecision{
                        Side::Buy, quantity, last_score_};
                }
            } else if (position == 0) {
                decision = StrategyDecision{
                    Side::Buy, config_.order_quantity, last_score_};
            }
        } else if (last_score_ <= -config_.entry_threshold) {
            if (position > 0) {
                const auto quantity = close_quantity(
                    position, tick.bid_quantity);
                if (quantity > 0) {
                    decision = StrategyDecision{
                        Side::Sell, quantity, last_score_};
                }
            } else if (position == 0) {
                decision = StrategyDecision{
                    Side::Sell, config_.order_quantity, last_score_};
            }
        } else if (std::abs(last_score_) <= config_.exit_threshold) {
            if (position > 0) {
                const auto quantity = close_quantity(
                    position, tick.bid_quantity);
                if (quantity > 0) {
                    decision = StrategyDecision{
                        Side::Sell, quantity, last_score_};
                }
            } else if (position < 0) {
                const auto quantity = close_quantity(
                    position, tick.ask_quantity);
                if (quantity > 0) {
                    decision = StrategyDecision{
                        Side::Buy, quantity, last_score_};
                }
            }
        }

        return decision;
    }

  private:
    static constexpr double external_bias_scale = 1'000'000.0;
    static_assert(std::atomic<std::int32_t>::is_always_lock_free,
                  "control-plane bias requires lock-free int32 atomics");

    [[nodiscard]] Quantity close_quantity(
        const Quantity position,
        const Quantity displayed_quantity) const noexcept {
        if (position == 0 || displayed_quantity <= 0) {
            return 0;
        }
        const auto position_magnitude =
            position < 0
                ? static_cast<std::uint64_t>(-(position + 1)) + 1U
                : static_cast<std::uint64_t>(position);
        const auto limit = std::min(
            static_cast<std::uint64_t>(config_.order_quantity),
            static_cast<std::uint64_t>(displayed_quantity));
        return static_cast<Quantity>(
            std::min(position_magnitude, limit));
    }

    StrategyConfig config_;
    double fast_ema_{};
    double slow_ema_{};
    std::atomic<std::int32_t> external_bias_scaled_{0};
    double last_score_{};
    std::uint64_t event_count_{};
    std::uint64_t last_order_event_{};
    bool initialized_{false};
    bool has_ordered_{false};
};

}  // namespace stockagent
