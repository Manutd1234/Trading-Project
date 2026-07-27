# StockAgent C++20 engine

This directory contains the dependency-free low-latency research and
paper-trading subsystem.

## Build

```bash
cmake -S cpp -B build/cpp \
  -DCMAKE_BUILD_TYPE=Release \
  -DSTOCKAGENT_BUILD_TESTS=ON
cmake --build build/cpp --parallel
ctest --test-dir build/cpp --output-on-failure
```

From the repository root, the same workflow is available as `make build` and
`make test`.

## Run

```bash
# Reproducible synthetic feed through the SPSC handoff
./build/cpp/stockagent_trader --events 10000 --seed 42

# Ordered top-of-book CSV replay
./build/cpp/stockagent_trader --csv cpp/data/sample_ticks.csv

# Optional static external bias supplied at startup
./build/cpp/stockagent_trader --events 10000 --external-bias 0.25
```

CSV files use integer prices and this exact header:

```text
sequence,timestamp_ns,bid_ticks,bid_quantity,ask_ticks,ask_quantity
```

Each engine instance handles one instrument. Sequence numbers and timestamps
must increase strictly.

## Select strategy parameters

For new research, prefer the causal chronological-validation backtester below.
The original optimizer is retained as a small single-split engine-parameter
example.

```bash
# Bounded training search plus a separate validation segment
./build/cpp/stockagent_strategy_optimizer --events 50000 --seed 42

# Use an ordered historical top-of-book CSV
./build/cpp/stockagent_strategy_optimizer \
  --csv path/to/ticks.csv \
  --train-percent 70 \
  --fee-ticks-per-fill 10
```

The optimizer searches only a documented grid of EMA windows, signal weights,
entry/exit thresholds, and cooldowns. It ranks a net liquidation-equity curve
that includes per-fill fees, closing spread/fee for terminal inventory, and
drawdown. Validation indicators receive no-trade pre-split warmup context. Its
holdout result is not a claim of globally optimal parameters or future returns;
use ordered historical data rather than the repeating synthetic demo before
drawing any conclusions.

## Causal chronological multi-window backtesting

```bash
# Public-research-inspired event-time heuristics and fixed ensembles
./build/cpp/stockagent_backtester --events 40000 --seed 42

# Ordered historical top-of-book data and explicit execution assumptions
./build/cpp/stockagent_backtester \
  --csv path/to/ticks.csv \
  --strategy auto \
  --folds 4 \
  --final-test-percent 20 \
  --embargo-events 64 \
  --latency-ns 50000 \
  --max-participation-bps 500 \
  --fee-ticks-per-unit 2 \
  --base-slippage-ticks 3 \
  --impact-coefficient 8

# JSON on stdout
./build/cpp/stockagent_backtester --csv path/to/ticks.csv --json
```

Decisions can fill only on a later event after the configured latency. The
future fill is capped by displayed-liquidity participation, crosses the future
BBO, pays quantity-aware fees, and includes configurable slippage and
square-root impact sensitivity. A causal tail stops new strategy decisions and
uses later events to flatten inventory; incomplete liquidation runs are
ineligible. Unavailable inventory-liquidation marks also make a validation run
ineligible. The final chronological segment is excluded from candidate
selection. A catalog candidate is recommended only when every validation-fold
score beats no trade; otherwise the output recommends `no_trade` while retaining
the rejected candidate's diagnostics. The report also shows a fixed
doubled-cost/latency scenario, which is not guaranteed to be a worst case, plus
buy-and-hold and no-trade comparisons. Buy-and-hold is not risk matched.

The bounded catalog is single-instrument and event-time; it does not implement
a firm's proprietary strategy or the multi-asset studies cited in the detailed
guide. Selection aggregates fixed candidates across chronological validation
windows. It is not purged CV, CPCV, PBO, Deflated Sharpe, or fold-by-fold model
retraining.

Reported Sharpe and Sortino values are unadjusted ratios of fixed-event-count
P&L buckets. They are not annualized or serial-correlation adjusted. Exposure
and time-in-market diagnostics are event weighted. Synthetic-feed output
validates plumbing only. See
[`../docs/institutional-backtesting.md`](../docs/institutional-backtesting.md)
for exact formulas, public research references, selection boundaries, and
limitations.

## Benchmark

```bash
./build/cpp/stockagent_latency_bench --warmup 20000 --events 500000
```

The benchmark measures uninstrumented throughput separately from sampled
latency, reports fill and no-fill p50/p95/p99/p99.9 distributions, and
reports the steady-clock baseline. Raw samples include timer overhead and
quantization. Do not use hosted-CI results as a performance guarantee.

Accounting uses checked integer arithmetic. If a market mark or ledger update
cannot be represented exactly, the engine enters a fail-closed arithmetic fault
and will not resume strategy processing.

See [`../docs/low-latency-engine.md`](../docs/low-latency-engine.md) for the
architecture, strategy, controls, limitations, and Python/LLM integration
boundary.
