#pragma once

#include <cstddef>
#include <vector>

namespace beaglex::analysis {

using Series = std::vector<double>;

// Non-overlapping log returns over `step` samples: ln(p[i+step] / p[i]).
Series log_returns(const Series& prices, std::size_t step);

Series absolute(const Series& x);

double mean(const Series& x);
double stddev(const Series& x); // sample standard deviation

// Fourth standardised moment minus 3: zero for a normal distribution,
// positive for fat tails.
double excess_kurtosis(const Series& x);

double correlation(const Series& a, const Series& b);

// Autocorrelation at `lag`, pooled over independent series (e.g. one per
// simulated day). Pairs never span two series, so day boundaries do not leak.
double autocorrelation(const std::vector<Series>& series, std::size_t lag);

// Hill estimator of the tail index alpha from the largest `tail_fraction` of
// the values (which must be positive). Smaller alpha means fatter tails;
// empirical asset returns typically give 2 to 5.
double hill_tail_index(Series values, double tail_fraction);

} // namespace beaglex::analysis
