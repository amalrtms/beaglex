// Simulates many independent trading days and tests the resulting prices
// against the stylized facts of real markets. The fundamental value, rounded
// to the same price grid, is analysed alongside as a control: it is a plain
// random walk, so any fact it lacks but prices show must come from the agents.
//
// Usage: beaglex_validate [--days N] [--seed S] [--out DIR]

#include "beaglex/analysis/stylized_facts.hpp"
#include "beaglex/sim/population.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

using namespace beaglex;
using namespace beaglex::analysis;

namespace {

constexpr std::size_t kMaxLag = 30;

double mean_of(const std::vector<double>& v, std::size_t from, std::size_t to) {
    double s = 0;
    for (std::size_t i = from; i < to; ++i) {
        s += v[i];
    }
    return s / static_cast<double>(to - from);
}

double max_abs(const std::vector<double>& v, std::size_t from, std::size_t to) {
    double m = 0;
    for (std::size_t i = from; i < to; ++i) {
        m = std::max(m, std::abs(v[i]));
    }
    return m;
}

struct Check {
    const char* fact;
    const char* metric;
    std::string price;
    std::string control;
    const char* real_markets;
    bool pass;
};

std::string fmt(const char* f, double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, f, v);
    return buf;
}

std::vector<Check> evaluate(const FactsReport& p, const FactsReport& c) {
    const double band = p.confidence_band;
    const auto& pk = p.kurtosis_by_scale;
    const auto& ck = c.kurtosis_by_scale;
    std::vector<Check> checks;

    // 1. Returns are not linearly predictable. Intraday lag-1 is allowed to
    //    be mildly negative (bid-ask bounce), as in real data.
    checks.push_back({"No return autocorrelation", "max |ACF| lags 1-10", fmt("%.3f", max_abs(p.acf, 0, 10)),
                      fmt("%.3f", max_abs(c.acf, 0, 10)), "< 0.1", max_abs(p.acf, 0, 10) < 0.1});

    // 2. Heavy tails at short horizons, beyond what price discreteness alone gives.
    checks.push_back({"Fat tails", "excess kurtosis, 1 min", fmt("%.2f", pk[0].excess_kurtosis),
                      fmt("%.2f", ck[0].excess_kurtosis), "> 0 (often 5-50)",
                      pk[0].excess_kurtosis > 1.0 && pk[0].excess_kurtosis > 2.0 * ck[0].excess_kurtosis});
    checks.push_back({"", "Hill tail index", fmt("%.2f", p.hill_alpha), fmt("%.2f", c.hill_alpha), "2 - 5",
                      p.hill_alpha >= 2.0 && p.hill_alpha <= 5.0});

    // 3. Tails thin out as the horizon grows.
    checks.push_back({"Aggregational Gaussianity", "kurtosis 1 min -> 30 min",
                      fmt("%.2f", pk[0].excess_kurtosis) + " -> " + fmt("%.2f", pk[3].excess_kurtosis),
                      fmt("%.2f", ck[0].excess_kurtosis) + " -> " + fmt("%.2f", ck[3].excess_kurtosis), "decreasing",
                      pk[3].excess_kurtosis < pk[0].excess_kurtosis / 2.0});

    // 4. Volatility clustering: |returns| are positively autocorrelated for
    //    many lags, and this must not be inherited from the fundamental.
    const double p_abs = mean_of(p.abs_acf, 0, 10);
    const double c_abs = mean_of(c.abs_acf, 0, 10);
    checks.push_back({"Volatility clustering", "mean ACF of |r|, lags 1-10", fmt("%.3f", p_abs), fmt("%.3f", c_abs),
                      "> 0, slow decay", p_abs > 3.0 * band && p.abs_acf[9] > band && c_abs < 2.0 * band});

    // 5. Volume and volatility move together.
    checks.push_back({"Volume-volatility correlation", "corr(volume, |r|) per minute",
                      fmt("%.3f", p.volume_volatility_corr), "n/a", "> 0", p.volume_volatility_corr > 3.0 * band});
    return checks;
}

void write_outputs(const std::filesystem::path& dir, const FactsReport& p, const FactsReport& c, const DayData& example) {
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir / "acf.csv");
        out << "lag_min,price_acf,price_abs_acf,control_acf,control_abs_acf,band\n";
        for (std::size_t i = 0; i < p.acf.size(); ++i) {
            out << i + 1 << ',' << p.acf[i] << ',' << p.abs_acf[i] << ',' << c.acf[i] << ',' << c.abs_acf[i] << ','
                << p.confidence_band << '\n';
        }
    }
    {
        std::ofstream out(dir / "kurtosis.csv");
        out << "minutes,price,control\n";
        for (std::size_t i = 0; i < p.kurtosis_by_scale.size(); ++i) {
            out << p.kurtosis_by_scale[i].minutes << ',' << p.kurtosis_by_scale[i].excess_kurtosis << ','
                << c.kurtosis_by_scale[i].excess_kurtosis << '\n';
        }
    }
    {
        std::ofstream out(dir / "returns.csv");
        out << "price,control\n";
        for (std::size_t i = 0; i < p.pooled_returns.size(); ++i) {
            out << p.pooled_returns[i] << ',' << c.pooled_returns[i] << '\n';
        }
    }
    {
        std::ofstream out(dir / "example_day.csv");
        out << "time_s,price,fundamental,volume\n";
        for (std::size_t i = 0; i < example.price.size(); ++i) {
            out << i << ',' << example.price[i] << ',' << example.fundamental[i] << ',' << example.volume[i] << '\n';
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    std::size_t days = 30;
    std::uint64_t seed = 1;
    std::filesystem::path out_dir = "out/validation";
    sim::PopulationConfig population;
    double jumps_per_hour = 0.0; // strict by default: every fat tail must be emergent, not injected by news
    double fundamental_vol = sim::FundamentalConfig{}.volatility_per_second;
    for (int i = 1; i + 1 < argc; i += 2) {
        const char* v = argv[i + 1];
        if (!std::strcmp(argv[i], "--days")) {
            days = std::strtoull(v, nullptr, 10);
        } else if (!std::strcmp(argv[i], "--seed")) {
            seed = std::strtoull(v, nullptr, 10);
        } else if (!std::strcmp(argv[i], "--out")) {
            out_dir = v;
        } else if (!std::strcmp(argv[i], "--noise")) {
            population.noise_traders = std::strtoull(v, nullptr, 10);
        } else if (!std::strcmp(argv[i], "--momentum")) {
            population.momentum_traders = std::strtoull(v, nullptr, 10);
        } else if (!std::strcmp(argv[i], "--value")) {
            population.value_traders = std::strtoull(v, nullptr, 10);
        } else if (!std::strcmp(argv[i], "--jumps-per-hour")) {
            jumps_per_hour = std::atof(v);
        } else if (!std::strcmp(argv[i], "--value-threshold-bp")) {
            population.value.threshold = std::atof(v) * 1e-4;
        } else if (!std::strcmp(argv[i], "--value-noise-bp")) {
            population.value.noise_sigma = std::atof(v) * 1e-4;
            population.value.bias_sigma = std::atof(v) * 0.5e-4;
        } else if (!std::strcmp(argv[i], "--value-interval-s")) {
            population.value.mean_interval = static_cast<sim::Time>(std::atof(v) * static_cast<double>(sim::kSecond));
        } else if (!std::strcmp(argv[i], "--fundamental-vol-bp")) {
            fundamental_vol = std::atof(v) * 1e-4; // per sqrt second
        } else if (!std::strcmp(argv[i], "--activity")) {
            population.noise.activity_sensitivity = std::atof(v);
        } else if (!std::strcmp(argv[i], "--mm-stress-spread")) {
            population.market_maker.stress_spread = std::atof(v);
        } else if (!std::strcmp(argv[i], "--market-makers")) {
            population.market_makers = std::strtoull(v, nullptr, 10);
        } else {
            std::fprintf(stderr, "usage: beaglex_validate [--days N] [--seed S] [--out DIR] [--noise N] "
                                 "[--momentum N] [--value N] [--jumps-per-hour X] [--value-threshold-bp X] "
                                 "[--value-noise-bp X] [--value-interval-s X] [--activity X] [--mm-stress-spread X] [--market-makers N]\n");
            return 2;
        }
    }
    const auto& vc = population.value;
    std::printf("Population: %zu market makers, %zu noise, %zu momentum, %zu value\n", population.market_makers,
                population.noise_traders, population.momentum_traders, population.value_traders);
    std::printf("Fundamental: %.2f bp per sqrt second, %.2f news jumps/hour\n", fundamental_vol * 1e4,
                jumps_per_hour);
    std::printf("Value traders: act on %.0f bp mispricing, %.0f bp estimation noise, wake every %.0f s\n",
                vc.threshold * 1e4, vc.noise_sigma * 1e4,
                static_cast<double>(vc.mean_interval) / static_cast<double>(sim::kSecond));
    std::printf("Feedback: noise activity sensitivity %.2f, market maker spread per unit volatility %.2f\n",
                population.noise.activity_sensitivity, population.market_maker.stress_spread);

    std::vector<Series> prices, controls, volumes;
    double traded = 0, market_maker_volume = 0;
    DayData example;
    std::printf("Simulating %zu trading days", days);
    for (std::size_t d = 0; d < days; ++d) {
        sim::SimConfig cfg;
        cfg.seed = seed + d;
        cfg.fundamental.jumps_per_hour = jumps_per_hour;
        cfg.fundamental.volatility_per_second = fundamental_vol;
        sim::Simulation s(cfg);
        sim::add_population(s, population);
        s.run();
        for (const auto& t : s.trades()) {
            traded += static_cast<double>(t.qty);
        }
        for (sim::AgentId id = 0; id < s.agent_count(); ++id) {
            if (std::string_view(s.agent(id).kind()) == "market_maker") {
                market_maker_volume += static_cast<double>(s.account(id).volume);
            }
        }
        DayData day = extract_day(s);
        prices.push_back(day.price);
        controls.push_back(round_to_half_tick(day.fundamental));
        volumes.push_back(day.volume);
        if (d == 0) {
            example = std::move(day);
        }
        std::printf(".");
        std::fflush(stdout);
    }
    std::printf("\n\n");

    const FactsReport p = analyse(prices, volumes, kMaxLag);
    const FactsReport c = analyse(controls, {}, kMaxLag);

    std::printf("Market makers are on one side of %.1f%% of traded shares.\n",
                traded > 0 ? 100.0 * market_maker_volume / traded : 0.0);
    std::printf("%zu one-minute returns. Price: sd %.1f bp, %.0f%% zero. Control: sd %.1f bp, %.0f%% zero.\n",
                p.returns, p.return_stddev * 1e4, p.zero_return_share * 100, c.return_stddev * 1e4,
                c.zero_return_share * 100);
    std::printf("95%% band for autocorrelations: +/-%.3f\n\n", p.confidence_band);

    const auto checks = evaluate(p, c);
    std::printf("%-30s %-28s %-16s %-16s %-17s %s\n", "Stylized fact", "Metric", "Price", "Control", "Real markets",
                "Result");
    std::printf("%s\n", std::string(118, '-').c_str());
    int passed = 0;
    for (const auto& ch : checks) {
        std::printf("%-30s %-28s %-16s %-16s %-17s %s\n", ch.fact, ch.metric, ch.price.c_str(), ch.control.c_str(),
                    ch.real_markets, ch.pass ? "PASS" : "FAIL");
        passed += ch.pass ? 1 : 0;
    }
    std::printf("\n%d of %zu checks passed.\n", passed, checks.size());

    write_outputs(out_dir, p, c, example);
    std::printf("Wrote acf.csv, kurtosis.csv, returns.csv, example_day.csv to %s\n", out_dir.string().c_str());
    return passed == static_cast<int>(checks.size()) ? 0 : 1;
}
