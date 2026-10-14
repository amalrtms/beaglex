#pragma once

#include "beaglex/sim/agents.hpp"
#include "beaglex/sim/simulation.hpp"

#include <cstddef>

namespace beaglex::sim {

struct PopulationConfig {
    std::size_t market_makers = 2;
    std::size_t noise_traders = 100;
    std::size_t momentum_traders = 20;
    std::size_t value_traders = 30;

    // Market makers are colocated; everyone else gets a latency drawn
    // uniformly from [min_latency, max_latency].
    Time market_maker_latency = 100 * kMicrosecond;
    Time min_latency = 1 * kMillisecond;
    Time max_latency = 20 * kMillisecond;

    MarketMakerConfig market_maker;
    NoiseTraderConfig noise;
    MomentumTraderConfig momentum;
    ValueTraderConfig value;
};

// Adds the population to `sim`. Agent seeds and latencies derive from the
// simulation seed, so the whole run is reproducible from that one number.
void add_population(Simulation& sim, PopulationConfig cfg);

} // namespace beaglex::sim
