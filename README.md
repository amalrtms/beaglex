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
| 1 | Matching engine: limit / market / IOC, cancel, modify, price-time priority | âœ… |
| 2 | Latency & throughput benchmarks, data-structure optimisation | â³ |
| 3 | Simulation loop with hand-coded agents (market makers, momentum, noise traders), fees, latency | â³ |
| 4 | Validation against empirical stylized facts (fat tails, volatility clustering, â€¦) | â³ |
| 5 | Genetic evolution of agent strategies | â³ |
| 6 | Experiments and write-up | â³ |

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

The current book uses `std::map` price levels with a `std::list` FIFO per level
and a hash index for O(1) cancels. It is intentionally the straightforward
baseline; Phase 2 benchmarks and optimises against it.

## Correctness

Besides unit tests, the engine is **differentially fuzzed** against a
deliberately naive reference book (`tests/reference_book.hpp`) that linearly
scans every order on every match. Both receive the same random stream of
submits, cancels and modifies (including invalid ones), and every result,
trade and book level must match after every operation. The fuzz test also
checks that the book is never left crossed.

## Building (Windows, MSVC)

Requires Visual Studio 2022+ or Build Tools with the C++ workload (includes CMake and Ninja).

```powershell
.\build.ps1                 # Release build with tests
.\build.ps1 -NoTests        # skip GoogleTest (no download)
.\build\release\beaglex_demo.exe
.\build\release\beaglex_tests.exe
```

## Limitations

Stated up front so results are read in the right light:

- Single instrument, single venue; no cross-venue effects or hidden/iceberg orders.
- No real market data yet; the agents generate all order flow.
- Agents are simplified models of real participants.
- Fees and latency will be modelled, but only approximately.

## Layout

```
include/beaglex/     public headers
src/                 engine implementation
apps/demo.cpp        walk-through of a sweep and a market order
tests/               unit tests, reference book, differential fuzz test
```
