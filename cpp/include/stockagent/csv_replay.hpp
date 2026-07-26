#pragma once

#include "stockagent/types.hpp"

#include <charconv>
#include <cstddef>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace stockagent {
namespace detail {

[[nodiscard]] inline std::vector<std::string_view> split_csv_line(
    const std::string_view line) {
    std::vector<std::string_view> fields;
    fields.reserve(6);
    std::size_t start = 0;
    while (start <= line.size()) {
        const auto comma = line.find(',', start);
        if (comma == std::string_view::npos) {
            fields.emplace_back(line.substr(start));
            break;
        }
        fields.emplace_back(line.substr(start, comma - start));
        start = comma + 1;
    }
    return fields;
}

template <typename T>
[[nodiscard]] inline T parse_integer_field(
    const std::string_view text, const std::size_t line_number) {
    T value{};
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        throw std::runtime_error("invalid integer on CSV line " +
                                 std::to_string(line_number));
    }
    return value;
}

}  // namespace detail

[[nodiscard]] inline std::vector<MarketTick> load_market_ticks_csv(
    const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open CSV file: " + path);
    }

    std::string line;
    if (!std::getline(input, line)) {
        throw std::runtime_error("CSV file is empty: " + path);
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    if (line !=
        "sequence,timestamp_ns,bid_ticks,bid_quantity,ask_ticks,"
        "ask_quantity") {
        throw std::runtime_error(
            "unexpected CSV header; see cpp/data/sample_ticks.csv");
    }

    std::vector<MarketTick> ticks;
    std::size_t line_number = 1;
    MarketTick previous{};
    bool has_previous = false;
    while (std::getline(input, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        const auto fields = detail::split_csv_line(line);
        if (fields.size() != 6) {
            throw std::runtime_error("expected 6 fields on CSV line " +
                                     std::to_string(line_number));
        }
        const MarketTick tick{
            detail::parse_integer_field<Sequence>(fields[0], line_number),
            detail::parse_integer_field<TimestampNs>(fields[1], line_number),
            detail::parse_integer_field<Price>(fields[2], line_number),
            detail::parse_integer_field<Quantity>(fields[3], line_number),
            detail::parse_integer_field<Price>(fields[4], line_number),
            detail::parse_integer_field<Quantity>(fields[5], line_number),
        };
        if (tick.bid_ticks <= 0 || tick.ask_ticks <= 0 ||
            tick.bid_ticks > tick.ask_ticks || tick.bid_quantity <= 0 ||
            tick.ask_quantity <= 0) {
            throw std::runtime_error(
                "invalid top-of-book values on CSV line " +
                std::to_string(line_number));
        }
        if (has_previous &&
            (tick.sequence <= previous.sequence ||
             tick.timestamp_ns <= previous.timestamp_ns)) {
            throw std::runtime_error(
                "CSV sequence and timestamp must increase on line " +
                std::to_string(line_number));
        }
        ticks.push_back(tick);
        previous = tick;
        has_previous = true;
    }
    if (input.bad()) {
        throw std::runtime_error("I/O failure while reading CSV file: " +
                                 path);
    }
    if (ticks.empty()) {
        throw std::runtime_error("CSV file contains no events: " + path);
    }
    return ticks;
}

}  // namespace stockagent
