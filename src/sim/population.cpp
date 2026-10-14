#include "beaglex/sim/population.hpp"

#include <memory>
#include <random>

namespace beaglex::sim {

namespace {

// splitmix64: decorrelates seeds derived from consecutive indices.
std::uint64_t derive_seed(std::uint64_t base, std::uint64_t index) {
    std::uint64_t x = base + 0x9e3779b97f4a7c15ULL * (index + 1);
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

} // namespace

void add_population(Simulation& sim, PopulationConfig cfg) {
    const std::uint64_t base = sim.config().seed;
    const Price initial = sim.config().fundamental.initial;
    cfg.market_maker.fallback_price = initial;
    cfg.noise.fallback_price = initial;

    std::mt19937_64 latency_rng(derive_seed(base, 0xC0FFEE));
    std::uniform_int_distribution<Time> latency(cfg.min_latency, cfg.max_latency);
    std::uint64_t index = 0;

    for (std::size_t i = 0; i < cfg.market_makers; ++i) {
        sim.add_agent(std::make_unique<MarketMaker>(cfg.market_maker, derive_seed(base, index++)),
                      cfg.market_maker_latency);
    }
    for (std::size_t i = 0; i < cfg.noise_traders; ++i) {
        sim.add_agent(std::make_unique<NoiseTrader>(cfg.noise, derive_seed(base, index++)), latency(latency_rng));
    }
    for (std::size_t i = 0; i < cfg.momentum_traders; ++i) {
        sim.add_agent(std::make_unique<MomentumTrader>(cfg.momentum, derive_seed(base, index++)), latency(latency_rng));
    }
    for (std::size_t i = 0; i < cfg.value_traders; ++i) {
        sim.add_agent(std::make_unique<ValueTrader>(cfg.value, derive_seed(base, index++)), latency(latency_rng));
    }
}

} // namespace beaglex::sim
