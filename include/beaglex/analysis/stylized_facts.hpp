#pragma once

// Tests simulated prices against the stylized facts of real asset returns
// (Cont, "Empirical properties of asset returns: stylized facts and
// statistical issues", Quantitative Finance, 2001).

#include "beaglex/analysis/stats.hpp"
#include "beaglex/sim/simulation.hpp"

#include <cstddef>
#include <vector>

namespace beaglex::analysis {

// One simulated day at one-second resolution.
struct DayData {
    Series price;       // mid price in ticks, gaps forward-filled
    Series fundamental; // hidden true value in ticks
    Series volume;      // shares traded during each second
};

DayData extract_day(const sim::Simulation& sim);

// Rounds a series to the nearest half tick, the resolution of a mid price.
// Used to build a control with the same price discreteness as the market.
Series round_to_half_tick(const Series& x);

struct ScaleKurtosis {
    std::size_t minutes = 0;
    std::size_t observations = 0;
    double excess_kurtosis = 0;
};

struct FactsReport {
    std::size_t days = 0;
    std::size_t returns = 0;        // pooled one-minute returns
    double return_stddev = 0;       // of one-minute returns
    double zero_return_share = 0;   // fraction of one-minute returns that are exactly zero
    double confidence_band = 0;     // 95% band for an autocorrelation under no dependence
    std::vector<double> acf;        // one-minute returns, lags 1..max_lag
    std::vector<double> abs_acf;    // |one-minute returns|, lags 1..max_lag
    std::vector<ScaleKurtosis> kurtosis_by_scale;
    double hill_alpha = 0;          // from the top 5% of |one-minute returns|
    double volume_volatility_corr = 0; // per-minute volume vs |return|; NaN without volume
    Series pooled_returns;          // for plotting the distribution
};

// `volumes` may be empty (e.g. for the fundamental, which has no trading).
FactsReport analyse(const std::vector<Series>& prices, const std::vector<Series>& volumes, std::size_t max_lag);

} // namespace beaglex::analysis
