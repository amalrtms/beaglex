#pragma once

#include "beaglex/evo/evolved_trader.hpp"
#include "beaglex/evo/genome.hpp"
#include "beaglex/sim/population.hpp"
#include "beaglex/sim/simulation.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace beaglex::evo {

struct EvolutionConfig {
    std::size_t population = 20;
    std::size_t generations = 60;
    std::size_t days_per_generation = 4;
    std::size_t elites = 4;     // best genomes carried over unchanged
    std::size_t tournament = 3; // parent selection: best of this many random picks
    double crossover_rate = 0.7;
    double mutation_rate = 0.2;
    double mutation_scale = 0.1;
    // Fitness = mean daily PnL - risk_aversion * standard deviation of daily PnL.
    double risk_aversion = 0.5;
    std::uint64_t seed = 1;

    sim::SimConfig market;          // seed is overridden per simulated day
    sim::PopulationConfig background;
    // Fixed strategies added to every simulated day as part of the environment.
    // They trade like evolved agents but are not evolved or scored.
    std::vector<Genome> fixed_strategies;
    EvolvedTraderConfig trader;
    sim::Time evolved_latency = 5 * sim::kMillisecond;
};

// One simulated day with the whole evolving population trading in it.
struct DayResult {
    std::vector<double> pnl; // dollars per evolved agent, after liquidating the closing position
    std::vector<double> minute_returns; // pool across days for kurtosis: one day's is dominated by any news jump
    double return_sd_bp = 0;         // one-minute returns
    double abs_return_acf1 = 0;      // volatility clustering at one minute
    double avg_spread_ticks = 0;
    double evolved_volume_share = 0; // evolved agents' share of all agent-side volume (each trade has two sides)
    double max_deviation_pct = 0;    // largest |price / fundamental - 1| during the day, in percent
    double mean_deviation_bp = 0;    // average |price / fundamental - 1|, in basis points
    double fixed_pnl = 0;            // dollars per fixed strategy, averaged over them
    double halts = 0;                // trading halts triggered by price bands
};

struct Evaluation {
    std::vector<double> fitness; // per genome
    std::vector<DayResult> days;

    // Excess kurtosis of one-minute returns pooled over all days.
    double pooled_kurtosis() const;
    // Mean of a per-day statistic.
    double day_mean(double DayResult::*field) const;
};

struct GenerationStats {
    std::size_t generation = 0;
    double best_fitness = 0;
    double mean_fitness = 0;
    double median_fitness = 0;
    Genome best;
    std::array<double, kGeneCount> mean_genes{};
    std::array<std::size_t, kArchetypeCount> archetypes{};
    // Market behaviour, averaged over the generation's days.
    double return_sd_bp = 0;
    double excess_kurtosis = 0;
    double abs_return_acf1 = 0;
    double avg_spread_ticks = 0;
    double evolved_volume_share = 0;
    double max_deviation_pct = 0;
};

// A genetic algorithm in which the population co-evolves: all genomes trade
// in the same simulated market, against the background agents and each other.
class Evolution {
public:
    explicit Evolution(const EvolutionConfig& config);

    // Evaluates the current population, records its statistics, then breeds
    // the next generation.
    GenerationStats step();

    const std::vector<Genome>& population() const noexcept { return population_; }
    std::size_t generation() const noexcept { return generation_; }

    // Places `genomes` together in the market for `days` days starting at
    // `first_seed`, running days in parallel. Deterministic for given inputs.
    Evaluation evaluate(const std::vector<Genome>& genomes, std::uint64_t first_seed, std::size_t days) const;

private:
    DayResult simulate_day(const std::vector<Genome>& genomes, std::uint64_t seed) const;
    const Genome& tournament_pick(const std::vector<double>& fitness);

    EvolutionConfig config_;
    std::mt19937_64 rng_;
    std::vector<Genome> population_;
    std::size_t generation_ = 0;
};

} // namespace beaglex::evo
