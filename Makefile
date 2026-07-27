SHELL := /bin/sh

CMAKE ?= cmake
CTEST ?= ctest
BUILD_DIR ?= build/cpp
SANITIZE_BUILD_DIR ?= build/cpp-sanitize
BUILD_TYPE ?= Release
CMAKE_ARGS ?=
RUN_ARGS ?= --events 10000 --seed 42
BENCH_ARGS ?=
OPTIMIZE_ARGS ?=
BACKTEST_ARGS ?=
PARALLEL ?=

.DEFAULT_GOAL := help

.PHONY: configure build test run bench optimize backtest sanitize clean help

configure:
	$(CMAKE) -S cpp -B "$(BUILD_DIR)" \
		-DCMAKE_BUILD_TYPE="$(BUILD_TYPE)" \
		-DBUILD_TESTING=ON \
		-DSTOCKAGENT_BUILD_TESTS=ON $(CMAKE_ARGS)

build: configure
	$(CMAKE) --build "$(BUILD_DIR)" --parallel $(PARALLEL)

test: build
	$(CTEST) --test-dir "$(BUILD_DIR)" --output-on-failure

run: configure
	$(CMAKE) --build "$(BUILD_DIR)" --target stockagent_trader --parallel $(PARALLEL)
	"$(BUILD_DIR)/stockagent_trader" $(RUN_ARGS)

bench: configure
	$(CMAKE) --build "$(BUILD_DIR)" --target stockagent_latency_bench --parallel $(PARALLEL)
	"$(BUILD_DIR)/stockagent_latency_bench" $(BENCH_ARGS)

optimize: configure
	$(CMAKE) --build "$(BUILD_DIR)" --target stockagent_strategy_optimizer --parallel $(PARALLEL)
	"$(BUILD_DIR)/stockagent_strategy_optimizer" $(OPTIMIZE_ARGS)

backtest: configure
	$(CMAKE) --build "$(BUILD_DIR)" --target stockagent_backtester --parallel $(PARALLEL)
	"$(BUILD_DIR)/stockagent_backtester" $(BACKTEST_ARGS)

sanitize:
	$(CMAKE) -S cpp -B "$(SANITIZE_BUILD_DIR)" \
		-DCMAKE_BUILD_TYPE=Debug \
		-DBUILD_TESTING=ON \
		-DSTOCKAGENT_BUILD_TESTS=ON \
		-DSTOCKAGENT_ENABLE_ASAN_UBSAN=ON $(CMAKE_ARGS)
	$(CMAKE) --build "$(SANITIZE_BUILD_DIR)" --parallel $(PARALLEL)
	ASAN_OPTIONS=strict_string_checks=1 \
	UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
	$(CTEST) --test-dir "$(SANITIZE_BUILD_DIR)" --output-on-failure

clean:
	$(CMAKE) -E remove_directory "$(BUILD_DIR)"
	$(CMAKE) -E remove_directory "$(SANITIZE_BUILD_DIR)"

help:
	@echo "StockAgent C++ build helpers"
	@echo
	@echo "  make configure  Configure the C++ build"
	@echo "  make build      Build the engine, CLIs, tests, optimizer, and benchmark"
	@echo "  make test       Build and run CTest"
	@echo "  make run        Run the reproducible synthetic-feed demo"
	@echo "  make bench      Run the latency benchmark"
	@echo "  make optimize   Run train/holdout strategy parameter selection"
	@echo "  make backtest   Run causal walk-forward strategy research"
	@echo "  make sanitize   Build and test with AddressSanitizer and UBSan"
	@echo "  make clean      Remove normal and sanitizer build directories"
	@echo
	@echo "Useful overrides:"
	@echo "  BUILD_TYPE=Debug BUILD_DIR=build/debug PARALLEL=4"
	@echo "  RUN_ARGS='--csv path/to/ticks.csv' BENCH_ARGS='...'"
	@echo "  OPTIMIZE_ARGS='--csv path/to/ticks.csv'"
	@echo "  BACKTEST_ARGS='--csv path/to/ticks.csv --strategy auto'"
	@echo "  CMAKE_ARGS='-DCMAKE_CXX_COMPILER=clang++'"
