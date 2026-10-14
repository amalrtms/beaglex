// Statistics are checked against distributions with known answers.

#include "beaglex/analysis/stats.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <random>

using namespace beaglex::analysis;

namespace {

Series draw(std::size_t n, auto&& dist, std::uint64_t seed = 1) {
    std::mt19937_64 rng(seed);
    Series x(n);
    for (auto& v : x) {
        v = dist(rng);
    }
    return x;
}

} // namespace

TEST(Stats, LogReturnsAreNonOverlapping) {
    const Series p = {100, 110, 121, 133.1, 146.41};
    const Series r = log_returns(p, 2);
    ASSERT_EQ(r.size(), 2u);
    EXPECT_NEAR(r[0], std::log(1.21), 1e-12);
    EXPECT_NEAR(r[1], std::log(1.21), 1e-12);
}

TEST(Stats, NormalHasZeroExcessKurtosis) {
    const Series x = draw(200'000, std::normal_distribution<double>(0, 1));
    EXPECT_NEAR(excess_kurtosis(x), 0.0, 0.05);
    EXPECT_NEAR(stddev(x), 1.0, 0.01);
}

TEST(Stats, LaplaceHasExcessKurtosisThree) {
    // Difference of two exponentials is Laplace distributed.
    std::exponential_distribution<double> e(1.0);
    const Series x = draw(400'000, [&](auto& rng) { return e(rng) - e(rng); });
    EXPECT_NEAR(excess_kurtosis(x), 3.0, 0.2);
}

TEST(Stats, AutocorrelationOfAr1) {
    // x[t] = 0.5 x[t-1] + noise has ACF 0.5^k.
    std::mt19937_64 rng(3);
    std::normal_distribution<double> noise(0, 1);
    Series x(200'000);
    for (std::size_t i = 1; i < x.size(); ++i) {
        x[i] = 0.5 * x[i - 1] + noise(rng);
    }
    EXPECT_NEAR(autocorrelation({x}, 1), 0.5, 0.01);
    EXPECT_NEAR(autocorrelation({x}, 2), 0.25, 0.01);
}

TEST(Stats, PooledAutocorrelationIgnoresSeriesBoundaries) {
    // Each series is constant on opposite sides of the pooled mean (0), so
    // every within-series lag-1 pair contributes +1: 6 pairs / variance 8 = 0.75.
    // A pair spanning the boundary would contribute -1 and give 5/8.
    const std::vector<Series> s = {{1, 1, 1, 1}, {-1, -1, -1, -1}};
    EXPECT_NEAR(autocorrelation(s, 1), 0.75, 1e-12);
}

TEST(Stats, IidNoiseHasNoAutocorrelation) {
    const Series x = draw(100'000, std::normal_distribution<double>(0, 1));
    for (std::size_t lag = 1; lag <= 5; ++lag) {
        EXPECT_LT(std::abs(autocorrelation({x}, lag)), 0.01);
    }
}

TEST(Stats, HillRecoversParetoTailIndex) {
    // Pareto with alpha = 3: x = u^(-1/alpha).
    std::uniform_real_distribution<double> u(0.0, 1.0);
    const Series x = draw(200'000, [&](auto& rng) { return std::pow(1.0 - u(rng), -1.0 / 3.0); });
    EXPECT_NEAR(hill_tail_index(x, 0.05), 3.0, 0.15);
}

TEST(Stats, CorrelationOfLinearRelation) {
    const Series a = draw(50'000, std::normal_distribution<double>(0, 1));
    Series b(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        b[i] = 2 * a[i] + 1;
    }
    EXPECT_NEAR(correlation(a, b), 1.0, 1e-9);
}
