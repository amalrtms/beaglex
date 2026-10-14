#include "beaglex/evo/genome.hpp"

#include <algorithm>
#include <cmath>

namespace beaglex::evo {

void Genome::clamp() noexcept {
    for (std::size_t i = 0; i < kGeneCount; ++i) {
        genes[i] = std::clamp(genes[i], kGeneSpecs[i].lo, kGeneSpecs[i].hi);
    }
}

Genome Genome::random(std::mt19937_64& rng) {
    Genome g;
    for (std::size_t i = 0; i < kGeneCount; ++i) {
        g.genes[i] = std::uniform_real_distribution<double>(kGeneSpecs[i].lo, kGeneSpecs[i].hi)(rng);
    }
    return g;
}

Genome crossover(const Genome& a, const Genome& b, std::mt19937_64& rng) {
    std::bernoulli_distribution coin(0.5);
    Genome child;
    for (std::size_t i = 0; i < kGeneCount; ++i) {
        child.genes[i] = coin(rng) ? a.genes[i] : b.genes[i];
    }
    return child;
}

void mutate(Genome& g, double rate, double scale, std::mt19937_64& rng) {
    std::bernoulli_distribution hit(rate);
    std::normal_distribution<double> step(0.0, 1.0);
    for (std::size_t i = 0; i < kGeneCount; ++i) {
        if (hit(rng)) {
            g.genes[i] += step(rng) * scale * (kGeneSpecs[i].hi - kGeneSpecs[i].lo);
        }
    }
    g.clamp();
}

Archetype classify(const Genome& g) noexcept {
    if (g.quotes()) {
        return Archetype::MarketMaker;
    }
    // Momentum and reversion signals both measure trend: a positive weight
    // buys strength, a negative one sells it.
    const double trend = g[kMomentum] + g[kReversion];
    const double flow = g[kImbalance];
    if (std::abs(flow) > std::abs(trend)) {
        return flow > 0 ? Archetype::FlowFollower : Archetype::FlowFader;
    }
    return trend > 0 ? Archetype::TrendFollower : Archetype::MeanReverter;
}

const char* to_string(Archetype a) noexcept {
    switch (a) {
    case Archetype::MarketMaker: return "market_maker";
    case Archetype::TrendFollower: return "trend_follower";
    case Archetype::MeanReverter: return "mean_reverter";
    case Archetype::FlowFollower: return "flow_follower";
    case Archetype::FlowFader: return "flow_fader";
    }
    return "?";
}

} // namespace beaglex::evo
