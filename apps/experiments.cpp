// Controlled experiments on the simulated market.
//
//   beaglex_experiment algo-share        how much algorithmic trading can the market absorb?
//   beaglex_experiment herding           is the damage from algorithms, or from identical ones?
//   beaglex_experiment circuit-breakers  do exchange price bands contain algorithm-driven swings?
//
// Every condition runs on the same market seeds (a paired design), so
// differences between conditions come from the treatment, not from luck.
// Options: --days N (default 20), --out DIR (default out/experiments)

#include "beaglex/evo/evolution.hpp"
#include "beaglex/evo/presets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <fstream>
#include <string>
#include <vector>

using namespace beaglex;
using namespace beaglex::evo;

namespace {

constexpr std::uint64_t kFirstSeed = 5'001;
constexpr double kCrashPct = 2.0; // a day whose price strays this far from value counts as a dislocation

struct Stat {
    double mean = 0;
    double ci = 0; // 95% half-width
};

Stat summarise(const std::vector<double>& x) {
    Stat s;
    for (double v : x) {
        s.mean += v / static_cast<double>(x.size());
    }
    double ss = 0;
    for (double v : x) {
        ss += (v - s.mean) * (v - s.mean);
    }
    if (x.size() > 1) {
        s.ci = 1.96 * std::sqrt(ss / static_cast<double>(x.size() - 1)) / std::sqrt(static_cast<double>(x.size()));
    }
    return s;
}

struct Outcome {
    std::string label;
    Stat volatility_bp;
    Stat mean_deviation_bp;
    Stat max_deviation_pct;
    Stat spread_ticks;
    Stat algo_pnl;   // dollars per injected algorithm per day
    Stat algo_share; // of all agent-side volume
    double dislocation_days = 0; // fraction of days with max deviation > kCrashPct
    double kurtosis = 0;         // pooled one-minute returns
    double halts_per_day = 0;
};

Outcome run(const std::string& label, const std::vector<Genome>& algos, const sim::PriceBandConfig& band, std::size_t days) {
    EvolutionConfig cfg;
    cfg.market.price_band = band;
    const Evolution harness(cfg);
    const Evaluation ev = harness.evaluate(algos, kFirstSeed, days);

    std::vector<double> vol, dev, maxdev, spread, pnl, share;
    std::size_t dislocations = 0;
    for (const DayResult& d : ev.days) {
        vol.push_back(d.return_sd_bp);
        dev.push_back(d.mean_deviation_bp);
        maxdev.push_back(d.max_deviation_pct);
        spread.push_back(d.avg_spread_ticks);
        share.push_back(100 * d.evolved_volume_share);
        double day_pnl = 0;
        for (double p : d.pnl) {
            day_pnl += p / static_cast<double>(std::max<std::size_t>(1, d.pnl.size()));
        }
        pnl.push_back(day_pnl);
        dislocations += d.max_deviation_pct > kCrashPct ? 1 : 0;
    }
    Outcome o;
    o.label = label;
    o.volatility_bp = summarise(vol);
    o.mean_deviation_bp = summarise(dev);
    o.max_deviation_pct = summarise(maxdev);
    o.spread_ticks = summarise(spread);
    o.algo_pnl = summarise(pnl);
    o.algo_share = summarise(share);
    o.dislocation_days = static_cast<double>(dislocations) / static_cast<double>(days);
    o.kurtosis = ev.pooled_kurtosis();
    o.halts_per_day = ev.day_mean(&DayResult::halts);
    return o;
}

void print_header() {
    std::printf("%-28s %14s %14s %14s %8s %12s %16s %10s %7s\n", "Condition", "volatility bp", "|dev| bp", "max dev %",
                "disloc.", "spread", "algo $/day", "algo vol", "halts");
}

void print(const Outcome& o) {
    std::printf("%-28s %6.2f +/-%5.2f %6.1f +/-%5.1f %6.2f +/-%5.2f %7.0f%% %5.2f +/-%4.2f %8.0f +/-%5.0f %8.1f%% %7.1f\n",
                o.label.c_str(), o.volatility_bp.mean, o.volatility_bp.ci, o.mean_deviation_bp.mean,
                o.mean_deviation_bp.ci, o.max_deviation_pct.mean, o.max_deviation_pct.ci, 100 * o.dislocation_days,
                o.spread_ticks.mean, o.spread_ticks.ci, o.algo_pnl.mean, o.algo_pnl.ci, o.algo_share.mean, o.halts_per_day);
    std::fflush(stdout);
}

void write_csv(const std::filesystem::path& path, const std::vector<std::pair<std::string, Outcome>>& rows) {
    std::ofstream out(path);
    out << "group,condition,volatility_bp,volatility_ci,mean_deviation_bp,mean_deviation_ci,max_deviation_pct,"
           "max_deviation_ci,dislocation_share,spread_ticks,spread_ci,algo_pnl,algo_pnl_ci,algo_share_pct,kurtosis,halts_per_day\n";
    for (const auto& [group, o] : rows) {
        out << group << ",\"" << o.label << "\"," << o.volatility_bp.mean << ',' << o.volatility_bp.ci << ','
            << o.mean_deviation_bp.mean << ',' << o.mean_deviation_bp.ci << ',' << o.max_deviation_pct.mean << ','
            << o.max_deviation_pct.ci << ',' << o.dislocation_days << ',' << o.spread_ticks.mean << ','
            << o.spread_ticks.ci << ',' << o.algo_pnl.mean << ',' << o.algo_pnl.ci << ',' << o.algo_share.mean << ','
            << o.kurtosis << ',' << o.halts_per_day << '\n';
    }
}

int algo_share(std::size_t days, const std::filesystem::path& out_dir) {
    std::printf("Experiment 1: market quality as algorithms take a growing share of trading\n");
    std::printf("%zu paired market days per condition; +/- is a 95%% confidence interval. Dislocation: price > %.0f%% from value.\n\n",
                days, kCrashPct);
    std::vector<std::pair<std::string, Outcome>> rows;
    const sim::PriceBandConfig no_band;
    const std::size_t counts[] = {0, 5, 10, 20, 40, 80};

    struct Kind {
        const char* group;
        const char* name;
        Genome genome;
    };
    const Kind kinds[] = {{"momentum", "momentum algos", presets::momentum_algo()},
                          {"market_maker", "market-making algos", presets::evolved_market_maker()}};
    for (const Kind& k : kinds) {
        std::printf("-- %s (fast liquidity %s)\n", k.name, std::strcmp(k.group, "momentum") == 0 ? "takers" : "providers");
        print_header();
        for (std::size_t n : counts) {
            const Outcome o = run(std::to_string(n) + " " + k.name, std::vector<Genome>(n, k.genome), no_band, days);
            print(o);
            rows.emplace_back(k.group, o);
        }
        std::printf("\n");
    }
    write_csv(out_dir / "algo_share.csv", rows);
    return 0;
}

// The same number of momentum algorithms, either identical or each with its
// own threshold, speed and signal weights. Isolates herding from algorithmic
// trading as such.
std::vector<Genome> diversified_momentum(std::size_t n, bool vary_speed) {
    std::mt19937_64 rng(2024);
    auto u = [&](double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(rng); };
    std::vector<Genome> out;
    for (std::size_t i = 0; i < n; ++i) {
        Genome g = presets::momentum_algo();
        g[kMomentum] = u(0.5, 1.0);
        g[kReversion] = u(0.5, 1.0);
        g[kImbalance] = u(0.0, 0.5);
        g[kThreshold] = u(0.3, 1.5);
        const double interval = u(0.0, 1.5); // 1 to ~30 seconds; drawn either way so other genes match
        if (vary_speed) {
            g[kLogInterval] = interval;
        }
        out.push_back(g);
    }
    return out;
}

int herding(std::size_t days, const std::filesystem::path& out_dir) {
    std::printf("Experiment 1b: is it algorithms, or identical algorithms?\n");
    std::printf("Identical momentum algos vs the same number with their own signal weights and thresholds,\n");
    std::printf("trading at the same speed (every second) or also at varied speeds (1-30 s).\n");
    std::printf("%zu paired days per condition.\n\n", days);
    std::vector<std::pair<std::string, Outcome>> rows;
    print_header();
    const sim::PriceBandConfig no_band;
    for (std::size_t n : {std::size_t{10}, std::size_t{20}, std::size_t{40}}) {
        const Outcome same = run(std::to_string(n) + " identical", std::vector<Genome>(n, presets::momentum_algo()), no_band, days);
        print(same);
        rows.emplace_back("identical", same);
        // Same speed as the identical algos: only signals and thresholds differ.
        const Outcome same_speed =
            run(std::to_string(n) + " diverse, same speed", diversified_momentum(n, false), no_band, days);
        print(same_speed);
        rows.emplace_back("diverse_same_speed", same_speed);
        const Outcome diverse = run(std::to_string(n) + " diverse, varied speed", diversified_momentum(n, true), no_band, days);
        print(diverse);
        rows.emplace_back("diverse_varied_speed", diverse);
    }
    write_csv(out_dir / "herding.csv", rows);
    return 0;
}

int circuit_breakers(std::size_t days, const std::filesystem::path& out_dir) {
    std::printf("Experiment 2: do exchange price bands contain algorithm-driven swings?\n");
    std::printf("Bands cap executions at +/- width around the mid of 5 minutes earlier; with halts, an attempt to\n");
    std::printf("trade beyond the band pauses trading for 5 minutes instead. %zu paired days per condition.\n\n",
                days);
    std::vector<std::pair<std::string, Outcome>> rows;
    struct Band {
        const char* name;
        sim::PriceBandConfig cfg;
    };
    constexpr sim::Time lag = 5 * sim::kMinute;
    constexpr sim::Time pause = 5 * sim::kMinute;
    const Band bands[] = {{"no bands", {false, 0.0, lag, 0}},
                          {"5% bands", {true, 0.05, lag, 0}},
                          {"2% bands", {true, 0.02, lag, 0}},
                          {"5% bands + halts", {true, 0.05, lag, pause}},
                          {"2% bands + halts", {true, 0.02, lag, pause}}};
    for (std::size_t n : {std::size_t{0}, std::size_t{40}}) {
        std::printf("-- %zu momentum algos\n", n);
        print_header();
        for (const Band& b : bands) {
            const Outcome o = run(std::to_string(n) + " algos, " + b.name, std::vector<Genome>(n, presets::momentum_algo()),
                                  b.cfg, days);
            print(o);
            rows.emplace_back(n == 0 ? "calm" : "algo_heavy", o);
        }
        std::printf("\n");
    }
    write_csv(out_dir / "circuit_breakers.csv", rows);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: beaglex_experiment algo-share|herding|circuit-breakers [--days N] [--out DIR]\n");
        return 2;
    }
    std::size_t days = 20;
    std::filesystem::path out_dir = "out/experiments";
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--days")) {
            days = std::strtoull(argv[i + 1], nullptr, 10);
        } else if (!std::strcmp(argv[i], "--out")) {
            out_dir = argv[i + 1];
        }
    }
    std::filesystem::create_directories(out_dir);

    const std::string which = argv[1];
    if (which == "algo-share") {
        return algo_share(days, out_dir);
    }
    if (which == "herding") {
        return herding(days, out_dir);
    }
    if (which == "circuit-breakers") {
        return circuit_breakers(days, out_dir);
    }
    std::fprintf(stderr, "unknown experiment '%s'\n", which.c_str());
    return 2;
}
