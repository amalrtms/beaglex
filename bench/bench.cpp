// Throughput and latency benchmark for the order book implementations.
//
// Every book replays the identical pre-generated operation stream, so results
// are directly comparable and a checksum confirms they all did the same work.
// Usage: beaglex_bench [ops] [seed]

#include "beaglex/baseline_order_book.hpp"
#include "beaglex/order_book.hpp"
#include "workload.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

using namespace beaglex;
using namespace beaglex::bench;

namespace {

// ---- timing ----------------------------------------------------------------

// The CPU timestamp counter: ~0.4ns resolution versus 100ns for
// std::chrono::steady_clock on Windows, which is too coarse for sub-microsecond
// operations. The fences stop the CPU reordering work across the read.
inline std::uint64_t cycles() noexcept {
    _mm_lfence();
    const std::uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}

double calibrate_ns_per_cycle() {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    const auto c0 = cycles();
    while (clock::now() - t0 < std::chrono::milliseconds(300)) {
    }
    const auto c1 = cycles();
    const auto t1 = clock::now();
    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    return ns / static_cast<double>(c1 - c0);
}

std::uint64_t timer_overhead_cycles() {
    std::vector<std::uint64_t> samples(10'000);
    for (auto& s : samples) {
        const auto a = cycles();
        s = cycles() - a;
    }
    std::nth_element(samples.begin(), samples.begin() + samples.size() / 2, samples.end());
    return samples[samples.size() / 2];
}

void pin_to_one_core() {
#if defined(_WIN32)
    // Pin to a single performance core and raise priority so the scheduler
    // does not migrate the thread (or move it to an efficiency core) mid-run.
    SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << 2);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
}

// ---- replay ----------------------------------------------------------------

template <class Book>
inline std::uint64_t apply(Book& book, const Op& op) {
    switch (op.kind) {
    case Op::Kind::Submit: {
        const auto r = book.submit(op.order);
        return static_cast<std::uint64_t>(r.filled) * 31 + r.trades.size();
    }
    case Op::Kind::Cancel:
        return book.cancel(op.order.id) ? 1 : 0;
    case Op::Kind::Modify: {
        const auto r = book.modify(op.order.id, op.order.price, op.order.qty);
        return static_cast<std::uint64_t>(r.filled) * 31 + r.trades.size();
    }
    }
    return 0;
}

template <class Book>
void prefill(Book& book, const Workload& w) {
    // A real venue sizes its pools at startup; do the same where supported.
    if constexpr (requires { book.reserve(std::size_t{}); }) {
        book.reserve(std::size_t{1} << 16);
    }
    for (const auto& op : w.prefill) {
        apply(book, op);
    }
}

struct Result {
    std::string name;
    double mops = 0;          // million operations per second
    double p50 = 0, p90 = 0, p99 = 0, p999 = 0, max = 0; // nanoseconds
    std::uint64_t checksum = 0;
};

template <class Book>
Result run(const char* name, const Workload& w, double ns_per_cycle, std::uint64_t overhead) {
    Result res{name};

    // Throughput: time the whole stream at once (no per-op timer cost).
    // Best of several runs, which filters out OS noise.
    constexpr int kRuns = 5;
    for (int run = 0; run < kRuns; ++run) {
        Book book;
        prefill(book, w);
        std::uint64_t checksum = 0;
        const auto c0 = cycles();
        for (const auto& op : w.ops) {
            checksum += apply(book, op);
        }
        const auto c1 = cycles();
        const double seconds = static_cast<double>(c1 - c0) * ns_per_cycle * 1e-9;
        res.mops = std::max(res.mops, static_cast<double>(w.ops.size()) / seconds / 1e6);
        res.checksum = checksum;
    }

    // Latency: time every operation individually.
    std::vector<std::uint64_t> samples(w.ops.size());
    {
        Book book;
        prefill(book, w);
        std::uint64_t sink = 0;
        for (std::size_t i = 0; i < w.ops.size(); ++i) {
            const auto c0 = cycles();
            sink += apply(book, w.ops[i]);
            const auto c1 = cycles();
            samples[i] = c1 - c0 > overhead ? c1 - c0 - overhead : 0;
        }
        if (sink != res.checksum) {
            std::fprintf(stderr, "%s: checksum mismatch between runs\n", name);
            std::exit(1);
        }
    }
    std::sort(samples.begin(), samples.end());
    auto pct = [&](double p) {
        const auto i = std::min(samples.size() - 1, static_cast<std::size_t>(p * static_cast<double>(samples.size())));
        return static_cast<double>(samples[i]) * ns_per_cycle;
    };
    res.p50 = pct(0.50);
    res.p90 = pct(0.90);
    res.p99 = pct(0.99);
    res.p999 = pct(0.999);
    res.max = static_cast<double>(samples.back()) * ns_per_cycle;
    return res;
}

} // namespace

int main(int argc, char** argv) {
    WorkloadConfig cfg;
    if (argc > 1) {
        cfg.ops = std::strtoull(argv[1], nullptr, 10);
    }
    if (argc > 2) {
        cfg.seed = std::strtoull(argv[2], nullptr, 10);
    }

    pin_to_one_core();

    std::printf("Generating workload: %zu ops after %zu prefill orders (seed %llu)...\n", cfg.ops, cfg.prefill,
                static_cast<unsigned long long>(cfg.seed));
    WorkloadStats stats;
    const Workload w = generate_workload(cfg, &stats);
    std::printf("  mix: %.1f%% submit, %.1f%% cancel, %.1f%% modify; %zu trades; book holds %.0f orders on average\n\n",
                100.0 * static_cast<double>(stats.submits - cfg.prefill) / static_cast<double>(w.ops.size()),
                100.0 * static_cast<double>(stats.cancels) / static_cast<double>(w.ops.size()),
                100.0 * static_cast<double>(stats.modifies) / static_cast<double>(w.ops.size()), stats.trades,
                stats.avg_orders);

    const double ns_per_cycle = calibrate_ns_per_cycle();
    const auto overhead = timer_overhead_cycles();
    std::printf("Timer: %.3f GHz TSC, %llu cycles overhead subtracted per sample\n\n", 1.0 / ns_per_cycle,
                static_cast<unsigned long long>(overhead));

    const std::vector<Result> results = {
        run<BaselineOrderBook>("BaselineOrderBook", w, ns_per_cycle, overhead),
        run<OrderBook>("OrderBook", w, ns_per_cycle, overhead),
    };

    std::printf("| %-18s | %10s | %8s | %8s | %8s | %8s | %10s |\n", "Book", "Mops/s", "p50 ns", "p90 ns", "p99 ns",
                "p99.9 ns", "max ns");
    std::printf("|%s|%s|%s|%s|%s|%s|%s|\n", "--------------------", "-----------:", "---------:", "---------:",
                "---------:", "---------:", "-----------:");
    for (const auto& r : results) {
        std::printf("| %-18s | %10.2f | %8.0f | %8.0f | %8.0f | %8.0f | %10.0f |\n", r.name.c_str(), r.mops, r.p50,
                    r.p90, r.p99, r.p999, r.max);
    }

    std::printf("\nSpeedup vs baseline: %.2fx throughput, %.2fx p99 latency\n", results[1].mops / results[0].mops,
                results[0].p99 / results[1].p99);

    for (const auto& r : results) {
        if (r.checksum != results[0].checksum) {
            std::fprintf(stderr, "ERROR: %s did different work than %s (checksum mismatch)\n", r.name.c_str(),
                         results[0].name.c_str());
            return 1;
        }
    }
    std::printf("Checksums match: all books produced identical results.\n");
    return 0;
}
