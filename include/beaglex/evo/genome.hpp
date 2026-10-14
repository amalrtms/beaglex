#pragma once

// An interpretable trading strategy encoded as a fixed vector of bounded,
// named genes. Every gene has a direct meaning, so evolved strategies can be
// read and explained rather than treated as black boxes.

#include <array>
#include <cstddef>
#include <random>

namespace beaglex::evo {

enum Gene : std::size_t {
    kMomentum,    // weight on the trend signal (fast vs slow moving average)
    kReversion,   // weight on price stretched from its short moving average
    kImbalance,   // weight on top-of-book size imbalance
    kInventory,   // weight on own position (negative leans against inventory)
    kThreshold,   // |score| needed before trading directionally
    kAggression,  // probability of taking liquidity rather than joining the touch
    kLots,        // order size in lots
    kLogInterval, // log10 of the mean seconds between decisions
    kQuoteMode,   // > 0.5: quote both sides (market making) instead of trading directionally
    kHalfSpread,  // quoting mode: ticks either side of the score-skewed centre
    kGeneCount
};

struct GeneSpec {
    const char* name;
    double lo;
    double hi;
};

inline constexpr std::array<GeneSpec, kGeneCount> kGeneSpecs{{
    {"w_momentum", -1.0, 1.0},
    {"w_reversion", -1.0, 1.0},
    {"w_imbalance", -1.0, 1.0},
    {"w_inventory", -1.0, 1.0},
    {"threshold", 0.05, 2.0},
    {"aggression", 0.0, 1.0},
    {"lots", 1.0, 5.0},
    {"log10_interval_s", 0.0, 2.1}, // 1 s to ~2 minutes
    {"quote_mode", 0.0, 1.0},       // a random genome quotes with probability 1/2
    {"half_spread", 0.5, 5.0},
}};

struct Genome {
    std::array<double, kGeneCount> genes{};

    double operator[](Gene g) const noexcept { return genes[g]; }
    double& operator[](Gene g) noexcept { return genes[g]; }

    // Whether the strategy runs in two-sided quoting (market making) mode.
    bool quotes() const noexcept { return genes[kQuoteMode] > 0.5; }

    void clamp() noexcept;

    static Genome random(std::mt19937_64& rng);
};

// Uniform crossover: each gene comes from either parent with equal probability.
Genome crossover(const Genome& a, const Genome& b, std::mt19937_64& rng);

// Each gene mutates with probability `rate`, by a Gaussian step of `scale`
// times its range, and is clamped back into bounds.
void mutate(Genome& g, double rate, double scale, std::mt19937_64& rng);

// Human-readable archetype, from the dominant signal weight.
enum class Archetype { MarketMaker, TrendFollower, MeanReverter, FlowFollower, FlowFader };
inline constexpr std::size_t kArchetypeCount = 5;

Archetype classify(const Genome& g) noexcept;
const char* to_string(Archetype a) noexcept;

} // namespace beaglex::evo
