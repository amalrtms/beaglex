#include "beaglex/analysis/stats.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace beaglex::analysis {

Series log_returns(const Series& prices, std::size_t step) {
    Series out;
    if (step == 0) {
        return out;
    }
    for (std::size_t i = 0; i + step < prices.size(); i += step) {
        out.push_back(std::log(prices[i + step] / prices[i]));
    }
    return out;
}

Series absolute(const Series& x) {
    Series out(x.size());
    std::transform(x.begin(), x.end(), out.begin(), [](double v) { return std::abs(v); });
    return out;
}

double mean(const Series& x) {
    if (x.empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    double s = 0;
    for (double v : x) {
        s += v;
    }
    return s / static_cast<double>(x.size());
}

double stddev(const Series& x) {
    if (x.size() < 2) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double m = mean(x);
    double ss = 0;
    for (double v : x) {
        ss += (v - m) * (v - m);
    }
    return std::sqrt(ss / static_cast<double>(x.size() - 1));
}

double excess_kurtosis(const Series& x) {
    const double m = mean(x);
    double m2 = 0, m4 = 0;
    for (double v : x) {
        const double d2 = (v - m) * (v - m);
        m2 += d2;
        m4 += d2 * d2;
    }
    m2 /= static_cast<double>(x.size());
    m4 /= static_cast<double>(x.size());
    return m4 / (m2 * m2) - 3.0;
}

double correlation(const Series& a, const Series& b) {
    const std::size_t n = std::min(a.size(), b.size());
    const double ma = mean(a), mb = mean(b);
    double sab = 0, saa = 0, sbb = 0;
    for (std::size_t i = 0; i < n; ++i) {
        sab += (a[i] - ma) * (b[i] - mb);
        saa += (a[i] - ma) * (a[i] - ma);
        sbb += (b[i] - mb) * (b[i] - mb);
    }
    return sab / std::sqrt(saa * sbb);
}

double autocorrelation(const std::vector<Series>& series, std::size_t lag) {
    double total = 0;
    std::size_t count = 0;
    for (const Series& s : series) {
        for (double v : s) {
            total += v;
        }
        count += s.size();
    }
    const double m = total / static_cast<double>(count);

    double cov = 0, var = 0;
    for (const Series& s : series) {
        for (std::size_t i = 0; i < s.size(); ++i) {
            var += (s[i] - m) * (s[i] - m);
            if (i + lag < s.size()) {
                cov += (s[i] - m) * (s[i + lag] - m);
            }
        }
    }
    return cov / var;
}

double hill_tail_index(Series values, double tail_fraction) {
    std::sort(values.begin(), values.end(), std::greater<>());
    const auto k = static_cast<std::size_t>(tail_fraction * static_cast<double>(values.size()));
    if (k < 2 || k >= values.size() || values[k] <= 0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    double s = 0;
    for (std::size_t i = 0; i < k; ++i) {
        s += std::log(values[i] / values[k]);
    }
    return static_cast<double>(k) / s;
}

} // namespace beaglex::analysis
