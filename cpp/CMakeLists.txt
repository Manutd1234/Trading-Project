cmake_minimum_required(VERSION 3.20)

project(stockagent_low_latency_engine VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)

option(STOCKAGENT_BUILD_TESTS "Build the dependency-free test suite" ON)
option(STOCKAGENT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(STOCKAGENT_ENABLE_ASAN_UBSAN
       "Instrument targets with AddressSanitizer and UBSan" OFF)
option(STOCKAGENT_ENABLE_TSAN
       "Instrument targets with ThreadSanitizer" OFF)

if(STOCKAGENT_ENABLE_ASAN_UBSAN AND STOCKAGENT_ENABLE_TSAN)
    message(FATAL_ERROR "ASan/UBSan and TSan must use separate builds")
endif()
if((STOCKAGENT_ENABLE_ASAN_UBSAN OR STOCKAGENT_ENABLE_TSAN) AND MSVC)
    message(FATAL_ERROR "Configured sanitizers require GCC or Clang")
endif()

find_package(Threads REQUIRED)

add_library(stockagent_engine
    src/engine.cpp
)
add_library(stockagent::engine ALIAS stockagent_engine)

target_compile_features(stockagent_engine PUBLIC cxx_std_20)
target_include_directories(stockagent_engine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

function(stockagent_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        if(STOCKAGENT_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
            -Wshadow
        )
        if(STOCKAGENT_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

function(stockagent_enable_sanitizers target)
    if(STOCKAGENT_ENABLE_ASAN_UBSAN)
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
            -fno-sanitize-recover=all
        )
        target_link_options(${target} PRIVATE
            -fsanitize=address,undefined
        )
    elseif(STOCKAGENT_ENABLE_TSAN)
        target_compile_options(${target} PRIVATE
            -fsanitize=thread
            -fno-omit-frame-pointer
        )
        target_link_options(${target} PRIVATE
            -fsanitize=thread
        )
    endif()
endfunction()

stockagent_enable_warnings(stockagent_engine)
stockagent_enable_sanitizers(stockagent_engine)

add_library(stockagent_backtest
    src/backtest.cpp
)
add_library(stockagent::backtest ALIAS stockagent_backtest)
target_compile_features(stockagent_backtest PUBLIC cxx_std_20)
target_include_directories(stockagent_backtest
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
stockagent_enable_warnings(stockagent_backtest)
stockagent_enable_sanitizers(stockagent_backtest)

add_executable(stockagent_trader apps/trader.cpp)
target_link_libraries(stockagent_trader PRIVATE stockagent::engine Threads::Threads)
stockagent_enable_warnings(stockagent_trader)
stockagent_enable_sanitizers(stockagent_trader)

add_executable(stockagent_latency_bench benchmarks/latency.cpp)
target_link_libraries(stockagent_latency_bench PRIVATE stockagent::engine)
stockagent_enable_warnings(stockagent_latency_bench)
stockagent_enable_sanitizers(stockagent_latency_bench)

add_executable(stockagent_strategy_optimizer apps/optimizer.cpp)
target_link_libraries(stockagent_strategy_optimizer PRIVATE stockagent::engine)
stockagent_enable_warnings(stockagent_strategy_optimizer)
stockagent_enable_sanitizers(stockagent_strategy_optimizer)

add_executable(stockagent_backtester apps/backtester.cpp)
target_link_libraries(stockagent_backtester PRIVATE stockagent::backtest)
stockagent_enable_warnings(stockagent_backtester)
stockagent_enable_sanitizers(stockagent_backtester)

include(CTest)
if(BUILD_TESTING AND STOCKAGENT_BUILD_TESTS)
    add_executable(stockagent_tests tests/tests.cpp)
    target_link_libraries(stockagent_tests PRIVATE stockagent::engine Threads::Threads)
    stockagent_enable_warnings(stockagent_tests)
    stockagent_enable_sanitizers(stockagent_tests)

    add_executable(stockagent_backtest_tests tests/backtest_tests.cpp)
    target_link_libraries(
        stockagent_backtest_tests PRIVATE stockagent::backtest
    )
    stockagent_enable_warnings(stockagent_backtest_tests)
    stockagent_enable_sanitizers(stockagent_backtest_tests)

    add_test(NAME stockagent_unit_tests COMMAND stockagent_tests)
    add_test(
        NAME stockagent_backtest_unit_tests
        COMMAND stockagent_backtest_tests
    )
    add_test(
        NAME stockagent_synthetic_smoke
        COMMAND stockagent_trader --events 2000 --seed 42 --quiet
    )
    add_test(
        NAME stockagent_csv_replay_smoke
        COMMAND stockagent_trader
            --csv ${CMAKE_CURRENT_SOURCE_DIR}/data/sample_ticks.csv
            --quiet
    )
    add_test(
        NAME stockagent_missing_csv_rejected
        COMMAND stockagent_trader
            --csv ${CMAKE_CURRENT_SOURCE_DIR}/data/does-not-exist.csv
            --quiet
    )
    set_tests_properties(stockagent_missing_csv_rejected
        PROPERTIES WILL_FAIL TRUE
    )
    add_test(
        NAME stockagent_invalid_csv_rejected
        COMMAND stockagent_trader
            --csv ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/invalid_ticks.csv
            --quiet
    )
    set_tests_properties(stockagent_invalid_csv_rejected
        PROPERTIES WILL_FAIL TRUE
    )
    add_test(
        NAME stockagent_optimizer_smoke
        COMMAND stockagent_strategy_optimizer
            --events 5000
            --seed 42
    )
    add_test(
        NAME stockagent_backtester_smoke
        COMMAND stockagent_backtester
            --events 6000
            --folds 3
            --final-test-percent 20
            --embargo-events 32
            --seed 42
    )
    add_test(
        NAME stockagent_backtester_json_smoke
        COMMAND
            ${CMAKE_COMMAND}
            -DBACKTESTER=$<TARGET_FILE:stockagent_backtester>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_backtester_json.cmake
    )
    add_test(
        NAME stockagent_legacy_minimum_fills_rejected
        COMMAND stockagent_backtester --minimum-fills 2
    )
    set_tests_properties(stockagent_legacy_minimum_fills_rejected
        PROPERTIES WILL_FAIL TRUE
    )
endif()
