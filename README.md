# Beaglex

An agent-based market simulator in C++20 for studying how trading strategies
co-evolve inside a realistic limit order book.

**What this is:** a research platform in the tradition of the Santa Fe
Artificial Stock Market and JPMorgan's ABIDES: a matching engine, a population
of trading agents, and a genetic algorithm that evolves their strategies, used
to ask questions about *market behaviour* (herding, liquidity, crashes, arms
races between strategies).

**What this is not:** a money-making trading bot. Strategies that do well here
are exploiting a simulated market; that says nothing about real-world
profitability. See [Limitations](#limitations).

## Status

| Phase | Scope | Status |
|---|---|---|
| 1 | Matching engine: limit / market / IOC, cancel, modify, price-time priority | ✅ |
| 2 | Latency & throughput benchmarks, data-structure optimisation | ✅ |
| 3 | Simulation loop with hand-coded agents (market makers, momentum, noise traders), fees, latency | ✅ |
| 4 | Validation against empirical stylized facts (fat tails, volatility clustering, …) | ⏳ |
| 5 | Genetic evolution of agent strategies | ⏳ |
| 6 | Experiments and write-up | ⏳ |

## Market simulation

A discrete-event simulation of a full trading day: 152 agents of four types
(market makers, noise traders, momentum traders and value traders) trade through
the matching engine. Prices emerge from their interaction around a hidden
fundamental value that only value traders can estimate.

- **Latency everywhere**: orders, fill reports and market data are all delayed
  by each agent's distance from the exchange (100 µs to 20 ms).
- **Realistic fees**: 0.30¢ taker fee, 0.20¢ maker rebate.
- **Deterministic**: one seed reproduces the entire day trade for trade.
- **Fast**: a 6.5 hour day (~1M events) simulates in about a quarter of a second.

```
beaglex_sim --seed 3
  price      open 100.03  close 99.73  high 102.40  low 98.21
  true value open 100.00  close 99.21
  avg spread 2.4 ticks, two-sided 100% of the time
```

Model, agents, calibration and limitations: [docs/simulation.md](docs/simulation.md).

## Performance

On a realistic synthetic order flow (2M operations, ~8,600 resting orders),
single core of an Intel i7-14650HX laptop:

| Book | Throughput | p50 | p99 | p99.9 |
|---|---:|---:|---:|---:|
| Baseline (`std::map` / `std::list` / `std::unordered_map`) | 6.1 M ops/s | 153 ns | 560 ns | 930 ns |
| Optimised `OrderBook` | **15.2 M ops/s** | **68 ns** | **205 ns** | **330 ns** |

**2.5x throughput and 2.7x lower p99 latency**, with zero heap allocation on
the hot path. The gain came from four measured steps: a reused trade buffer, a
pooled intrusive order queue, a custom open-addressing hash index, and
contiguous price levels. Method, workload, per-step results and caveats are in
[docs/benchmarks.md](docs/benchmarks.md).

## Matching engine

- **Price-time priority**: best price first, then first-in-first-out within a price level.
- **Trades execute at the resting (maker) order's price.**
- **Integer tick prices**: no floating point anywhere in the book.
- **Order types**: Limit (remainder rests), Market (remainder discarded), IOC.
- **Modify semantics** match real venues: reducing size keeps queue position;
  changing price or increasing size is a cancel/replace that loses priority.
- **Deterministic**: priority comes from arrival order, not wall-clock time, so a
  given input sequence always yields identical trades. This matters for
  reproducible simulation experiments.

Two implementations share one interface. `OrderBook` is the optimised engine.
`BaselineOrderBook` is the original straightforward version, kept as the
benchmark reference and as a second implementation for testing.

## Correctness

Both books are **differentially fuzzed** against a deliberately naive
reference book ([`tests/reference_book.hpp`](tests/reference_book.hpp)) that
linearly scans every order on every match. All receive the same random stream
of submits, cancels and modifies (including invalid ones), and every result,
trade and book level must match after every operation. Two fuzz profiles run:
a narrow price band where the book crosses constantly, and a wide one that grows
the book to thousands of orders across hundreds of levels. The fuzz test also
checks that the book is never left crossed. The custom hash index has its own
differential test against `std::unordered_map`.

The fuzz tests were checked by planting a bug (trades priced at the taker's
price instead of the maker's); it was caught within 22 operations.

## Building (Windows, MSVC)

Requires Visual Studio 2022+ or Build Tools with the C++ workload (includes CMake and Ninja).

```powershell
.\build.ps1                          # Release build: library, demo, tests, benchmark
.\build.ps1 -NoTests                 # skip GoogleTest (no download)
.\build\release\beaglex_demo.exe     # walk through a sweep and a market order
.\build\release\beaglex_tests.exe    # unit + fuzz tests
.\build\release\beaglex_bench.exe    # throughput and latency comparison
.\build\release\beaglex_sim.exe --seed 1 --out out   # simulate a day; writes CSVs to out/
```

## Limitations

Stated up front so results are read in the right light:

- Single instrument, single venue; no cross-venue effects or hidden/iceberg orders.
- No real market data yet; the agents generate all order flow.
- Agents are simplified models of real participants.
- Fees and latency will be modelled, but only approximately.
- Benchmarks measure the matching engine alone, on a synthetic workload.

## Layout

```
include/beaglex/      public headers (OrderBook, BaselineOrderBook, types)
include/beaglex/sim/  simulation: event loop, agents, population
src/                  implementations
apps/demo.cpp         walk-through of a sweep and a market order
apps/sim.cpp          run a simulated trading day, write CSVs
bench/                workload generator and benchmark
tests/                unit, fuzz and simulation tests
docs/                 benchmark and simulation write-ups
```
