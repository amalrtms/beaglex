#include "beaglex/analysis/stylized_facts.hpp"

#include <cmath>
#include <limits>
#include <optional>

namespace beaglex::analysis {

namespace {

constexpr std::size_t kSecondsPerMinute = 60;

} // namespace

DayData extract_day(const sim::Simulation& sim) {
    const auto& samples = sim.samples();
    DayData d;
    d.price.reserve(samples.size());
    d.fundamental.reserve(samples.size());

    // Back-fill the opening seconds before the first two-sided quote, then
    // forward-fill any later gaps.
    std::optional<double> first_mid;
    for (const auto& s : samples) {
        if ((first_mid = s.market.mid())) {
            break;
        }
    }
    double last = first_mid.value_or(static_cast<double>(sim.config().fundamental.initial));
    for (const auto& s : samples) {
        if (const auto m = s.market.mid()) {
            last = *m;
        }
        d.price.push_back(last);
        d.fundamental.push_back(s.fundamental);
    }

    d.volume.assign(samples.size(), 0.0);
    for (const auto& t : sim.trades()) {
        const auto second = static_cast<std::size_t>(t.time / sim::kSecond);
        if (second < d.volume.size()) {
            d.volume[second] += static_cast<double>(t.qty);
        }
    }
    return d;
}

Series round_to_half_tick(const Series& x) {
    Series out(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) {
        out[i] = std::round(x[i] * 2.0) / 2.0;
    }
    return out;
}

FactsReport analyse(const std::vector<Series>& prices, const std::vector<Series>& volumes, std::size_t max_lag) {
    FactsReport r;
    r.days = prices.size();

    std::vector<Series> daily_returns;
    std::vector<Series> daily_abs;
    for (const Series& p : prices) {
        daily_returns.push_back(log_returns(p, kSecondsPerMinute));
        daily_abs.push_back(absolute(daily_returns.back()));
        r.pooled_returns.insert(r.pooled_returns.end(), daily_returns.back().begin(), daily_returns.back().end());
    }
    r.returns = r.pooled_returns.size();
    r.return_stddev = stddev(r.pooled_returns);
    std::size_t zeros = 0;
    for (double v : r.pooled_returns) {
        zeros += v == 0.0 ? 1 : 0;
    }
    r.zero_return_share = static_cast<double>(zeros) / static_cast<double>(r.returns);
    r.confidence_band = 1.96 / std::sqrt(static_cast<double>(r.returns));

    for (std::size_t lag = 1; lag <= max_lag; ++lag) {
        r.acf.push_back(autocorrelation(daily_returns, lag));
        r.abs_acf.push_back(autocorrelation(daily_abs, lag));
    }

    for (std::size_t minutes : {1, 5, 15, 30}) {
        Series pooled;
        for (const Series& p : prices) {
            const Series x = log_returns(p, minutes * kSecondsPerMinute);
            pooled.insert(pooled.end(), x.begin(), x.end());
        }
        r.kurtosis_by_scale.push_back({minutes, pooled.size(), excess_kurtosis(pooled)});
    }

    Series nonzero_abs;
    for (double v : r.pooled_returns) {
        if (v != 0.0) {
            nonzero_abs.push_back(std::abs(v));
        }
    }
    r.hill_alpha = hill_tail_index(nonzero_abs, 0.05);

    r.volume_volatility_corr = std::numeric_limits<double>::quiet_NaN();
    if (volumes.size() == prices.size()) {
        Series minute_volume, minute_abs;
        for (std::size_t d = 0; d < prices.size(); ++d) {
            const Series& rets = daily_returns[d];
            for (std::size_t i = 0; i < rets.size(); ++i) {
                double v = 0;
                for (std::size_t s = i * kSecondsPerMinute; s < (i + 1) * kSecondsPerMinute; ++s) {
                    v += volumes[d][s];
                }
                minute_volume.push_back(v);
                minute_abs.push_back(std::abs(rets[i]));
            }
        }
        r.volume_volatility_corr = correlation(minute_volume, minute_abs);
    }
    return r;
}

} // namespace beaglex::analysis
