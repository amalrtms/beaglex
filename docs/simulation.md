# Market simulation

A discrete-event simulation of one trading day on one exchange, in the
tradition of agent-based market models (Santa Fe Artificial Stock Market,
Chiarella & Iori 2002, JPMorgan's ABIDES). Prices are not drawn from a random
process: they emerge from heterogeneous agents trading against each other
through the [matching engine](../README.md#matching-engine).

## Simulation core

- **Simulated clock** in nanoseconds. Events (agent wake-ups, order and cancel
  arrivals, fill deliveries, samples) are processed in `(time, sequence)` order,
  so a 6.5 hour day takes well under a second to run.
- **Latency in every direction.** Each agent has a one-way latency to the
  exchange. Orders and cancels arrive one latency after they are sent, fill
  reports come back one latency after the trade, and market data is stale by one
  latency: an agent at time *t* sees the top of book as it was at *t − latency*.
  Market makers are colocated (100 µs); other agents sit 1–20 ms away.
- **Fees.** Takers pay 0.30¢ per share and makers receive a 0.20¢ rebate, in the
  range of US equity exchanges. Money is tracked in thousandths of a tick so
  sub-cent fees are exact.
- **Accounts.** The exchange keeps each agent's cash, position, volume and fees.
  Agents only learn about their fills through delayed reports.
- **A hidden fundamental value**, a geometric random walk (about 1.5% daily
  volatility) with occasional news jumps. Only value traders observe it, with
  noise and a persistent private bias.
- **Deterministic.** Every random draw derives from one seed. The same seed and
  configuration reproduce the same day trade for trade.

## Agents

| Agent | Default count | Behaviour |
|---|---:|---|
| Market maker | 2 | Every ~100 ms, quotes a bid and ask 1.5 ticks either side of the mid, shifted against its inventory (0.002 ticks per share held). Requotes only when its prices change, so it keeps queue priority. Stops quoting a side at its 5,000 share limit. |
| Noise trader | 100 | Uninformed flow. Every ~20 s, cancels its working order and sends a random buy or sell: 15% market orders, otherwise limits up to 10 ticks behind or 2 through the mid. |
| Momentum trader | 20 | Chartist. Tracks fast (2 min) and slow (15 min) time-weighted moving averages of the mid. Buys when the fast is 5 bp above the slow, sells when below, unwinds when the trend fades. |
| Value trader | 30 | Fundamentalist. Every ~90 s, buys when the ask is 20 bp below its estimate of value and sells when the bid is 20 bp above, sized by the mispricing. Takes profit once price converges. Institutional size (4,000 share limit). |

All parameters live in config structs in
[`include/beaglex/sim/agents.hpp`](../include/beaglex/sim/agents.hpp) and
[`population.hpp`](../include/beaglex/sim/population.hpp).

## A typical day

`beaglex_sim --seed 3`:

```
Market
  price      open 100.03  close 99.73  high 102.40  low 98.21
  true value open 100.00  close 99.21
  trades     ~62,000
  avg spread 2.4 ticks, two-sided 100% of the time
  avg |price - true value| 37 bp
```

Across seeds 1–5 the price tracks the fundamental with an average deviation
of 33–40 bp, with episodes where it overshoots by 1–2% and is pulled back.
Value traders profit and noise traders lose, as microstructure theory predicts
for informed versus uninformed flow (Kyle 1985; Glosten & Milgrom 1985).
Market makers earn the spread plus rebates.

These are observations, not yet validation. Phase 4 tests the simulated
returns against the statistical regularities of real markets.

## Calibration notes

The first version gave value traders the same position limits as noise
traders. Thirty value traders could then hold at most 45,000 shares against
noise flow that drifted 30,000+ shares net in one direction. They saturated,
and the price detached from value by up to 2.4% for hours. Real informed
capital is far larger than individual retail flow, so value traders were given
institutional size and noise traders a smaller inventory cap.

## Tests

[`tests/sim_test.cpp`](../tests/sim_test.cpp) checks:

- **Latency**: with scripted agents, orders arrive, fills return and market
  data is observed at exactly the expected simulated times.
- **Conservation**: across a full simulated hour, positions sum to zero, agent
  cash plus exchange revenue sums to zero, and fees match revenue. (Verified to
  catch a planted sign error in the maker rebate.)
- **Determinism**: the same seed gives identical trades; a different seed does not.
- **Ownership**: an agent cannot cancel another agent's order.
- **Risk limits**: market makers stay within their position limits.

## Limitations

- One instrument, one venue, one day.
- No self-trade prevention: an agent's aggressive order can match its own resting order.
- Cash is unconstrained (no margin or funding model).
- Agents see top of book only, not full depth.
- Latency is fixed per agent, with no jitter or queueing at the exchange.
- Determinism holds per platform. The C++ standard library's random
  distributions are implementation-defined, so MSVC and GCC produce different
  (equally valid) days from the same seed.
