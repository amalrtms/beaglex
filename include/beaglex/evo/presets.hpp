#pragma once

// Named strategies used as fixed participants in experiments.

#include "beaglex/evo/genome.hpp"

namespace beaglex::evo::presets {

// A fast, aggressive trend chaser: buys strength and sells weakness every
// second, taking liquidity, and leans into order-flow imbalance. The kind of
// strategy that briefly dominated Phase 5 runs with large populations.
inline Genome momentum_algo() {
    Genome g;
    g[kMomentum] = 1.0;
    g[kReversion] = 1.0;
    g[kImbalance] = 0.5;
    g[kInventory] = 0.0;
    g[kThreshold] = 0.5;
    g[kAggression] = 1.0;
    g[kLots] = 5.0;
    g[kLogInterval] = 0.0; // decides every second
    g[kQuoteMode] = 0.0;
    g[kHalfSpread] = 0.5;
    return g;
}

// The market maker evolved in Phase 5 (seed 1, best on held-out days).
inline Genome evolved_market_maker() {
    Genome g;
    g[kMomentum] = -0.13;
    g[kReversion] = 0.93;
    g[kImbalance] = 0.33;
    g[kInventory] = -1.0;
    g[kThreshold] = 1.66;
    g[kAggression] = 0.97;
    g[kLots] = 4.56;
    g[kLogInterval] = 0.0;
    g[kQuoteMode] = 1.0;
    g[kHalfSpread] = 0.5;
    return g;
}

} // namespace beaglex::evo::presets
