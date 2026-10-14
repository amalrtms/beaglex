// Evolves a population of trading strategies inside the simulated market.
// Usage: beaglex_evolve [--generations N] [--population N] [--days N] [--seed S] [--out DIR]

#include "beaglex/evo/evolution.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <string>

using namespace beaglex;
using namespace beaglex::evo;

namespace {

constexpr std::uint64_t kHoldoutSeed = 9'000'000; // never used during evolution
constexpr std::size_t kHoldoutDays = 10;

double mean_of(const std::vector<double>& v) {
    return std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
}

void write_generation_header(std::ofstream& out) {
    out << "generation,best_fitness,mean_fitness,median_fitness";
    for (const auto& g : kGeneSpecs) {
        out << ",mean_" << g.name;
    }
    for (std::size_t a = 0; a < kArchetypeCount; ++a) {
        out << ",n_" << to_string(static_cast<Archetype>(a));
    }
    out << ",return_sd_bp,excess_kurtosis,abs_return_acf1,avg_spread_ticks,evolved_volume_share,max_deviation_pct\n";
}

void write_generation(std::ofstream& out, const GenerationStats& s) {
    out << s.generation << ',' << s.best_fitness << ',' << s.mean_fitness << ',' << s.median_fitness;
    for (double g : s.mean_genes) {
        out << ',' << g;
    }
    for (std::size_t n : s.archetypes) {
        out << ',' << n;
    }
    out << ',' << s.return_sd_bp << ',' << s.excess_kurtosis << ',' << s.abs_return_acf1 << ',' << s.avg_spread_ticks
        << ',' << s.evolved_volume_share << ',' << s.max_deviation_pct << '\n';
}

std::string mix_summary(const std::array<std::size_t, kArchetypeCount>& counts) {
    static const char* const kShort[] = {"MM", "trend", "revert", "flow+", "flow-"};
    std::string out;
    for (std::size_t a = 0; a < kArchetypeCount; ++a) {
        out += std::string(kShort[a]) + " " + std::to_string(counts[a]) + (a + 1 < kArchetypeCount ? ", " : "");
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    EvolutionConfig cfg;
    std::filesystem::path out_dir = "out/evolution";
    for (int i = 1; i + 1 < argc; i += 2) {
        const char* v = argv[i + 1];
        if (!std::strcmp(argv[i], "--generations")) {
            cfg.generations = std::strtoull(v, nullptr, 10);
        } else if (!std::strcmp(argv[i], "--population")) {
            cfg.population = std::strtoull(v, nullptr, 10);
        } else if (!std::strcmp(argv[i], "--days")) {
            cfg.days_per_generation = std::strtoull(v, nullptr, 10);
        } else if (!std::strcmp(argv[i], "--seed")) {
            cfg.seed = std::strtoull(v, nullptr, 10);
        } else if (!std::strcmp(argv[i], "--risk-aversion")) {
            cfg.risk_aversion = std::atof(v);
        } else if (!std::strcmp(argv[i], "--out")) {
            out_dir = v;
        } else {
            std::fprintf(stderr, "usage: beaglex_evolve [--generations N] [--population N] [--days N] [--seed S] "
                                 "[--risk-aversion X] [--out DIR]\n");
            return 2;
        }
    }

    std::filesystem::create_directories(out_dir);
    std::ofstream gens(out_dir / "generations.csv");
    write_generation_header(gens);

    Evolution evo(cfg);
    const std::vector<Genome> initial = evo.population();
    std::printf("Evolving %zu strategies for %zu generations, %zu market days each (seed %llu)\n\n", cfg.population,
                cfg.generations, cfg.days_per_generation, static_cast<unsigned long long>(cfg.seed));
    std::printf("%4s %10s %10s | %-46s | %7s %6s %6s %6s %7s\n", "gen", "best $", "mean $", "strategy mix", "vol bp",
                "kurt", "|r|ac", "share", "maxdev");

    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t g = 0; g < cfg.generations; ++g) {
        const GenerationStats s = evo.step();
        write_generation(gens, s);
        gens.flush();
        std::printf("%4zu %10.0f %10.0f | %-46s | %7.2f %6.2f %6.3f %5.1f%% %6.2f%%\n", s.generation, s.best_fitness,
                    s.mean_fitness, mix_summary(s.archetypes).c_str(), s.return_sd_bp, s.excess_kurtosis,
                    s.abs_return_acf1, 100 * s.evolved_volume_share, s.max_deviation_pct);
        std::fflush(stdout);
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("\nEvolution took %.0f s.\n\n", seconds);

    // Held-out test: market days never seen during evolution.
    const std::vector<Genome>& final_pop = evo.population();
    const Evaluation before = evo.evaluate(initial, kHoldoutSeed, kHoldoutDays);
    const Evaluation after = evo.evaluate(final_pop, kHoldoutSeed, kHoldoutDays);

    auto day_mean = [](const Evaluation& e, double DayResult::*field) { return e.day_mean(field); };
    std::printf("Held-out test (%zu unseen days)        initial random    final evolved\n", kHoldoutDays);
    std::printf("  mean fitness per strategy ($/day)  %14.0f %16.0f\n", mean_of(before.fitness), mean_of(after.fitness));
    std::printf("  best fitness ($/day)               %14.0f %16.0f\n",
                *std::max_element(before.fitness.begin(), before.fitness.end()),
                *std::max_element(after.fitness.begin(), after.fitness.end()));
    std::printf("  strategies with positive fitness   %14zu %16zu\n",
                static_cast<std::size_t>(std::count_if(before.fitness.begin(), before.fitness.end(), [](double f) { return f > 0; })),
                static_cast<std::size_t>(std::count_if(after.fitness.begin(), after.fitness.end(), [](double f) { return f > 0; })));
    std::printf("  one-minute volatility (bp)         %14.2f %16.2f\n", day_mean(before, &DayResult::return_sd_bp),
                day_mean(after, &DayResult::return_sd_bp));
    std::printf("  excess kurtosis (pooled)           %14.2f %16.2f\n", before.pooled_kurtosis(),
                after.pooled_kurtosis());
    std::printf("  |return| autocorrelation, lag 1    %14.3f %16.3f\n", day_mean(before, &DayResult::abs_return_acf1),
                day_mean(after, &DayResult::abs_return_acf1));
    std::printf("  average spread (ticks)             %14.2f %16.2f\n", day_mean(before, &DayResult::avg_spread_ticks),
                day_mean(after, &DayResult::avg_spread_ticks));
    std::printf("  evolved agents' share of volume    %13.1f%% %15.1f%%\n",
                100 * day_mean(before, &DayResult::evolved_volume_share),
                100 * day_mean(after, &DayResult::evolved_volume_share));
    std::printf("  largest price deviation from value %13.2f%% %15.2f%%\n",
                day_mean(before, &DayResult::max_deviation_pct), day_mean(after, &DayResult::max_deviation_pct));

    // Final population, ranked by held-out fitness.
    std::vector<std::size_t> order(final_pop.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return after.fitness[a] > after.fitness[b]; });
    std::ofstream pop(out_dir / "final_population.csv");
    pop << "rank,holdout_fitness,archetype";
    for (const auto& g : kGeneSpecs) {
        pop << ',' << g.name;
    }
    pop << '\n';
    for (std::size_t r = 0; r < order.size(); ++r) {
        const Genome& g = final_pop[order[r]];
        pop << r + 1 << ',' << after.fitness[order[r]] << ',' << to_string(classify(g));
        for (double v : g.genes) {
            pop << ',' << v;
        }
        pop << '\n';
    }

    std::printf("\nTop evolved strategies on held-out days:\n");
    std::printf("%4s %10s %-15s", "rank", "fitness $", "archetype");
    for (const auto& g : kGeneSpecs) {
        std::printf(" %8.8s", g.name + (g.name[0] == 'w' ? 2 : 0));
    }
    std::printf("\n");
    for (std::size_t r = 0; r < std::min<std::size_t>(5, order.size()); ++r) {
        const Genome& g = final_pop[order[r]];
        std::printf("%4zu %10.0f %-15s", r + 1, after.fitness[order[r]], to_string(classify(g)));
        for (double v : g.genes) {
            std::printf(" %8.2f", v);
        }
        std::printf("\n");
    }
    std::printf("\nWrote generations.csv and final_population.csv to %s\n", out_dir.string().c_str());
    return 0;
}
