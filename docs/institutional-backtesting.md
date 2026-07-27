# Causal strategy research and chronological validation

`stockagent_backtester` is the cold-path research companion to the low-latency
paper engine. It is designed to make a strategy harder to fool, not to maximize
the displayed return. The implementation uses public systematic-research ideas;
it does not reproduce any firm's proprietary strategy, data, execution system,
or expected performance.

## What changed

The original demo strategy observes a top-of-book event and can fill a paper
order on that same event. That behavior is useful for exercising the hot path,
but it is too optimistic for strategy research. The causal backtester instead
separates:

```text
past/current observations
        |
        v
strategy forecast -> target position -> pending intent
                                         |
                                         | later event + configured latency
                                         v
future BBO -> participation cap -> IOC fill/cancel
                                         |
                                         v
cost-aware ledger -> equity buckets -> validation report
```

A decision made on event `t` cannot fill on `t`, even when configured latency
is zero. It can fill only on a later sequence whose timestamp satisfies the
latency assumption. One outstanding target is replaceable; a marketable
immediate-or-cancel simulation fills only the permitted quantity and cancels
the remainder.

## Public research inspiration, not proprietary strategies

The bounded, hard-coded catalog contains 11 candidates across four families:

- `trend`: single-instrument, event-time fast/slow EMA trend at three speeds,
  scaled by recent raw tick-change volatility;
- `mean-reversion`: a single-series rolling-price contrarian z-score with a
  trend filter and maximum holding period;
- `order_book_imbalance`: smoothed top-of-book imbalance plus microprice
  displacement;
- `ensemble`: three fixed blends of trend, mean reversion, and imbalance.

Position targets are scaled down when observed volatility rises and then
bounded by the maximum absolute position. Each later execution slice is
separately bounded by the smaller maximum order quantity. Signals flatten
before reversing.

These are transparent project heuristics suited to the repository's
single-instrument event schema. They are not implementations of the cited
studies. The trend family is broadly informed by the publicly documented
time-series-momentum literature from [Moskowitz, Ooi, and
Pedersen](https://w4.stern.nyu.edu/facdir/lpederse/papers/TimeSeriesMomentum.pdf),
[AQR's long-history trend study](https://www.aqr.com/Insights/Research/Journal-Article/A-Century-of-Evidence-on-Trend-Following-Investing),
and [Man Group's discussion of combining trend
speeds](https://www.man.com/insights/need-for-speed-trend-following). Those
public studies examine medium-horizon signals across diversified futures and
forwards with portfolio-level risk allocation. They do not validate this
event-time EMA heuristic. The code does not copy a firm's parameters, data, or
execution model and does not claim comparable results.

Pairs trading, cross-sectional momentum, factor-neutral statistical arbitrage,
and portfolio risk parity need synchronized multi-instrument data and are
therefore intentionally deferred. Realistic market making needs trades,
order-level depth, queue position, cancels, and venue rules; top-of-book quotes
are not enough.

## Execution and transaction costs

For an eligible order, the simulator:

1. computes the difference between the delayed target and current position;
2. caps it by maximum order quantity;
3. caps it again by
   `floor(displayed_quantity * max_participation_bps / 10000)`;
4. crosses the future bid or ask;
5. moves the fill farther by
   `base_slippage + ceil(impact_coefficient * sqrt(q / displayed_quantity))`;
6. charges a fixed fee plus a per-unit fee; and
7. cancels any unfilled remainder.

The final `liquidation_tail_events` stop new strategy decisions and submit
causal flattening intents against later BBO events under the same latency,
maximum-order, and participation rules. A run that cannot actually reach zero
position inside that tail is marked incomplete and is ineligible for candidate
selection. During the path, open inventory is marked using a modeled
full-position liquidation assembled from permitted slice sizes; that mark is a
valuation convention, not proof that all slices were executable at one quote.
If the participation rule permits zero closing quantity at any inventory-bearing
event, `all_liquidation_marks_available` becomes false and the run is
ineligible even if liquidity later returns.

The ledger reports two related cost measures. `executed_fill_shortfall_ticks`
attributes completed fills relative to their decision mids:

- price movement between decision and future fill
  (`executed_latency_cost_ticks`);
- crossing the future mid to the BBO (`executed_spread_cost_ticks`);
- modeled movement beyond the BBO (`executed_slippage_cost_ticks`); and
- explicit fees (`executed_fees_ticks`).

`modeled_total_shortfall_ticks` is the difference between the frictionless
decision-mid ledger and the cost-aware liquidation ledger, including the
modeled closing cost of any residual inventory. Neither measure includes the
opportunity cost of canceled or unfilled target quantity, so neither is the
complete paper-versus-real implementation shortfall defined by
[Perold](https://doi.org/10.3905/jpm.1988.409150).

`forced_liquidation_fill_shortfall_ticks` is the portion of executed-fill
shortfall incurred by real fills during the reserved closing tail.
`residual_modeled_liquidation_cost_ticks` applies only when inventory remains
after that tail; such a run is reported for diagnosis but is never eligible for
selection.

The square-root term uses displayed top-of-book quantity and is a configurable
sensitivity heuristic, not a calibrated capacity model. It is neither the
daily-volume empirical specification in [Frazzini, Israel, and Moskowitz on
trading costs](https://pages.stern.nyu.edu/~afrazzin/pdf/Trading%20Cost%20of%20Asset%20Pricing%20Anomalies%20-%20Frazzini%2C%20Israel%20and%20Moskowitz.pdf)
nor the temporary/permanent impact and execution-risk model in [Almgren and
Chriss](https://doi.org/10.21314/JOR.2001.041). Those works motivate treating
cost, size, volatility, and execution risk explicitly; their models and
coefficients are not copied here.

## Chronological selection and held-out testing

The default run uses chronological data only:

1. reserve the final 20% as a held-out test;
2. leave a no-scoring gap before that test;
3. divide the earlier development region into chronological validation windows
   preceded by expanding historical prefixes;
4. warm indicators from preceding observations without allowing trades;
5. require every candidate to share the same execution, configured warmup,
   metric-bucket, and liquidation-tail assumptions, then evaluate every
   hard-coded candidate on every validation window;
6. make a window ineligible if it has too few closed trades, cannot complete
   its causal liquidation tail, or ends with a nonzero position;
7. rank by median window score, then worst-window score, with stable catalog
   order as the final exact-tie rule;
8. recommend the best catalog candidate only if every validation-window score
   is above the zero no-trade score; otherwise recommend `no_trade`; and
9. evaluate that recommendation and the best catalog candidate on the held-out
   final segment.

The selection score is:

```text
(net liquidation P&L
 - maximum drawdown
 - 0.25 * max(executed-fill shortfall, 0))
/ max(1, average absolute position quantity)
```

This is an ad hoc conservative project score, not an objective taken from the
cited literature. Net P&L already includes costs; the shortfall term is an
additional cost penalty.

This process is chronological multi-window candidate selection. It does not
fit or reselect a model independently inside each expanding prefix, and it is
not purged cross-validation, CPCV, or a computation of Probability of Backtest
Overfitting or [Deflated
Sharpe](https://www.davidhbailey.com/dhbpapers/deflated-sharpe.pdf). The gap is
excluded from trading and scoring, but its past observations may causally warm
indicators; it is not a data purge.

Candidate and candidate-window counts describe only programmatic evaluations
in the current run. They cannot count earlier human research, code revisions,
or parameter ideas. The bounded catalog and held-out segment reduce some
selection risk but do not prove the absence of overfitting. The
[Probability of Backtest Overfitting
paper](https://scholarworks.wmich.edu/math_pubs/42/) explains why repeated
strategy searches and ordinary holdouts can still produce false discoveries.
Once the final segment has influenced a decision, it is no longer held out.

The recommendation gate is determined only from validation. A held-out failure
does not retroactively change it; doing so would leak the final segment into
selection. When the gate rejects the catalog, the report still shows the best
rejected candidate's final-segment diagnostics, clearly separated from the
zero-position recommended outcome.

The report also evaluates the final segment with doubled fees,
slippage/impact, and latency. This is a fixed `2x` assumption scenario, not a
guaranteed worst case: different latency can occasionally improve a simulated
fill, and real cost relationships need not scale linearly. The cost-aware
buy-and-hold comparison uses a fixed target of 40 quantity units, the default
100-unit position cap, the default 25-unit order cap, and the selected
execution assumptions. It is not capital-, exposure-, or volatility-matched.
A zero-P&L no-trade reference is also shown.

## Metrics

The report includes:

- gross and net liquidation P&L plus the actual terminal-liquidation
  completion and all-path-mark-availability flags;
- executed-fill and modeled-total shortfall, forced-tail fill cost, residual
  modeled liquidation cost, and fill-cost attribution;
- maximum drawdown and drawdown duration;
- bucket P&L mean, volatility, downside deviation, worst bucket, and mean of
  the worst 5% of buckets when at least 40 buckets exist;
- unadjusted event-bucket mean/volatility and mean/downside-deviation ratios,
  labeled Sharpe and Sortino per square-root bucket;
- turnover, event-weighted average absolute position quantity,
  event-weighted time in market, intents, fills, per-order-or-capacity-limited
  partial fills, cancellations, closed-trade hit rate, and profit factor; and
- fold ranges, fold dispersion, best-catalog parameters, recommendation,
  programmatic evaluation count, final-test boundary, and an FNV-1a data
  fingerprint. The fingerprint is canonical bytewise FNV-1a over a defined
  little-endian serialization of each six-field market event.

Each metrics object exposes `causal_comparison_valid`. P&L comparisons should
be treated as invalid unless the run both completed its actual closing tail and
had a permitted modeled liquidation mark at every inventory-bearing event.
Configuration output distinguishes the configured minimum warmup from the
effective indicator warmup requirement.

The Sharpe and Sortino fields are descriptive ratios of fixed-event-count P&L
buckets. They are not returns on capital, not annualized, and not adjusted for
serial correlation; irregular market-event arrival also means that buckets can
span different clock durations. A final incomplete event bucket is excluded
rather than given the same weight as a full bucket. The lower-tail field is a
sample mean of the worst 5% of bucket P&Ls, where more negative is worse,
rather than a claim of a precisely estimated regulatory expected-shortfall
measure.

No annualized Sharpe is printed because an event count is not a calendar.
[Andrew Lo's work on Sharpe-ratio
statistics](https://alo.mit.edu/publications/page/18/) documents why naive
annualization and inference can be misleading when returns are serially
dependent. Metrics with inadequate observations or zero variance are reported
as `N/A`.

## Run

```bash
# Reproducible plumbing demonstration
make backtest

# Historical ordered top-of-book replay
BACKTEST_ARGS='--csv path/to/ticks.csv --strategy auto' make backtest

# User-supplied cost and latency assumptions
BACKTEST_ARGS='--csv path/to/ticks.csv \
  --fixed-fee-ticks 5 \
  --fee-ticks-per-unit 2 \
  --base-slippage-ticks 3 \
  --impact-coefficient 8 \
  --max-participation-bps 500 \
  --latency-ns 50000' make backtest

# Machine-readable stdout
BACKTEST_ARGS='--csv path/to/ticks.csv --json' make backtest
```

The CSV schema remains:

```text
sequence,timestamp_ns,bid_ticks,bid_quantity,ask_ticks,ask_quantity
```

Sequence and timestamp must increase strictly, prices must be positive and
noncrossed, and displayed quantities must be positive.

## Limits that still matter

- The bundled synthetic feed deliberately repeats regimes and aligns drift with
  imbalance. Its results validate code paths only.
- Top-of-book data cannot establish queue priority, hidden liquidity, partial
  depth consumption, venue routing, borrow, funding, dividends, rolls,
  corporate actions, or real market impact.
- The displayed-quantity slippage coefficient and fixed `2x` scenario are
  sensitivity assumptions, not calibrated capacity or worst-case guarantees.
- The current research ledger is single-instrument. Claims about diversified
  institutional portfolios require point-in-time multi-asset data.
- A realistic historical study must control survivorship, look-ahead,
  timestamp, session, symbol-mapping, and corporate-action biases outside this
  file format.
- Historical and sensitivity-scenario results do not guarantee future returns
  and are not investment advice.
