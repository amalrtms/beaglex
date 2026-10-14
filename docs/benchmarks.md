# Order book benchmarks

## Results

2,000,000 operations replayed through each book after 10,000 prefill orders.
Best throughput of 5 runs; latency percentiles from a separate run timing every
operation individually. Three runs with seed 42 shown; seed 7 gives the same picture.

| Book | Throughput | p50 | p90 | p99 | p99.9 |
|---|---:|---:|---:|---:|---:|
| `BaselineOrderBook` | 6.1 M ops/s | 153 ns | 263 ns | 560 ns | 930 ns |
| `OrderBook` | **15.2 M ops/s** | **68 ns** | **120 ns** | **205 ns** | **330 ns** |
| Improvement | **2.5x** | 2.3x | 2.2x | 2.7x | 2.8x |

Both books replay the identical operation stream and a checksum over every
result confirms they produce the same trades.

## How the speedup was reached

Each step was applied on its own and measured on the same workload and machine
(single runs, so treat small differences as noise):

| Step | Change | Throughput | p50 | p99 |
|---|---|---:|---:|---:|
| 0 | Baseline: `std::map` levels, `std::list` queues, `std::unordered_map` index, new `std::vector` of trades per call | 6.2 M/s | 155 ns | 566 ns |
| 1 | Reuse one trade buffer, return a `std::span` into it | 7.1 M/s | 149 ns | 383 ns |
| 2 | Orders in a recycled contiguous pool, queues linked by index instead of `std::list` | 9.6 M/s | 110 ns | 288 ns |
| 3 | Flat open-addressing hash index (linear probing, backward-shift delete) instead of `std::unordered_map` | 14.3 M/s | 74 ns | 214 ns |
| 4 | Price levels in a sorted vector per side, best price at the back, instead of `std::map` | 15.2 M/s | 68 ns | 205 ns |

The common thread is removing heap allocation and pointer chasing from the hot
path. After warm-up, `OrderBook` does no allocation at all: every container is
contiguous and reused, so most operations touch a handful of cache lines.

- **Step 1** mainly cut the tail: a fresh `std::vector` per call meant an
  allocation on every trade-producing order, and allocator slow paths show up at p99.
- **Step 2** removed one allocation and one deallocation per resting order.
- **Step 3** was the largest single win. 93% of operations look up an order id
  (duplicate check on submit, cancel, modify), and `std::unordered_map` stores
  each entry in its own heap node, so every lookup was a likely cache miss.
- **Step 4** helps less because the baseline's `std::map` was already only a
  few levels deep near the touch, but matching now reads only the end of a
  contiguous array.

## Workload

Generated deterministically by [`bench/workload.hpp`](../bench/workload.hpp):

- **Mix:** ~69% new orders, ~24% cancels, ~7% modifies. Real venues see far more
  cancels than trades; this mix keeps that character.
- **Passive orders** rest at an exponentially distributed distance from a
  randomly walking fair value (mean 8 ticks), so activity concentrates near
  the touch as in real books.
- **Aggressive orders** (~8%): marketable limits, IOC and market orders that
  sweep one or more levels. ~870k trades over the run.
- **Book depth:** about 8,600 resting orders on average.
- Cancels and modifies always target orders that are live at that point.

## Method

- Timing uses the CPU timestamp counter (`rdtsc` with `lfence`), calibrated
  against `steady_clock`. On Windows `steady_clock` ticks every 100 ns, too
  coarse for operations this short. The measured cost of the timer itself is
  subtracted from each sample.
- The benchmark thread is pinned to one performance core at high priority.
- Release build, MSVC 19.50, `/O2`.
- Machine: Intel Core i7-14650HX laptop, Windows 11.

## Caveats

- Single-threaded matching of one instrument; no networking, logging or risk
  checks are included in these numbers.
- The workload is synthetic. It is designed to be realistic in shape, but it
  is not real market data.
- Maximum latency is not reported. It is dominated by the operating system
  (interrupts, context switches) rather than by the book, and varies by
  orders of magnitude between runs.
- Laptop results depend on power settings and thermal state.

## Reproducing

```powershell
.\build.ps1
.\build\release\beaglex_bench.exe            # 2M ops, seed 42
.\build\release\beaglex_bench.exe 5000000 7  # custom op count and seed
```
