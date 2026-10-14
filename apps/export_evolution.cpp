// Runs the Phase 5 evolution and writes its history as JSON for the browser
// playground's evolution view: per-generation statistics and best genome,
// plus a held-out comparison of the initial and final populations.
// Usage: beaglex_export_evolution [--seeds N] [--generations N] [--out FILE]

#include "beaglex/evo/evolution.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <numeric>
#include <sstream>
#include <string>

using namespace beaglex;
using namespace beaglex::evo;

namespace {

constexpr std::uint64_t kHoldoutSeed = 9'000'000; // same held-out days as beaglex_evolve
constexpr std::size_t kHoldoutDays = 10;

// Four significant digits keep the file small without visible loss.
std::string num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4g", v);
    return buf;
}

std::string genes_json(const Genome& g) {
    std::string out = "[";
    for (std::size_t i = 0; i < kGeneCount; ++i) {
        out += num(g.genes[i]) + (i + 1 < kGeneCount ? "," : "]");
    }
    return out;
}

std::string run_json(std::uint64_t seed, std::size_t generations) {
    EvolutionConfig cfg;
    cfg.seed = seed;
    cfg.generations = generations;
    Evolution evo(cfg);
    const std::vector<Genome> initial = evo.population();

    std::ostringstream out;
    out << "{\"seed\":" << seed << ",\"generations\":[";
    for (std::size_t g = 0; g < generations; ++g) {
        const GenerationStats s = evo.step();
        out << (g ? "," : "") << "{\"best\":" << num(s.best_fitness) << ",\"mean\":" << num(s.mean_fitness)
            << ",\"median\":" << num(s.median_fitness) << ",\"mix\":[";
        for (std::size_t a = 0; a < kArchetypeCount; ++a) {
            out << s.archetypes[a] << (a + 1 < kArchetypeCount ? "," : "]");
        }
        out << ",\"spread\":" << num(s.avg_spread_ticks) << ",\"vol\":" << num(s.return_sd_bp)
            << ",\"share\":" << num(s.evolved_volume_share) << ",\"bestGenome\":" << genes_json(s.best)
            << ",\"bestType\":\"" << to_string(classify(s.best)) << "\"}";
        std::fprintf(stderr, "seed %llu: generation %zu/%zu\r", static_cast<unsigned long long>(seed), g + 1, generations);
    }

    const std::vector<Genome>& final_pop = evo.population();
    const Evaluation before = evo.evaluate(initial, kHoldoutSeed, kHoldoutDays);
    const Evaluation after = evo.evaluate(final_pop, kHoldoutSeed, kHoldoutDays);
    auto positive = [](const Evaluation& e) {
        return std::count_if(e.fitness.begin(), e.fitness.end(), [](double f) { return f > 0; });
    };
    auto mean = [](const std::vector<double>& v) { return std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size()); };
    out << "],\"holdout\":{\"days\":" << kHoldoutDays << ",\"population\":" << final_pop.size()
        << ",\"initialPositive\":" << positive(before) << ",\"evolvedPositive\":" << positive(after)
        << ",\"initialMeanFitness\":" << num(mean(before.fitness)) << ",\"evolvedMeanFitness\":" << num(mean(after.fitness))
        << ",\"initialSpread\":" << num(before.day_mean(&DayResult::avg_spread_ticks))
        << ",\"evolvedSpread\":" << num(after.day_mean(&DayResult::avg_spread_ticks)) << "}}";
    std::fprintf(stderr, "\n");
    return out.str();
}

} // namespace

int main(int argc, char** argv) {
    std::size_t seeds = 3;
    std::size_t generations = 100;
    std::string path = "web/src/data/evolution.json";
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--seeds")) {
            seeds = std::strtoull(argv[i + 1], nullptr, 10);
        } else if (!std::strcmp(argv[i], "--generations")) {
            generations = std::strtoull(argv[i + 1], nullptr, 10);
        } else if (!std::strcmp(argv[i], "--out")) {
            path = argv[i + 1];
        } else {
            std::fprintf(stderr, "usage: beaglex_export_evolution [--seeds N] [--generations N] [--out FILE]\n");
            return 2;
        }
    }

    std::ofstream out(path);
    out << "{\"genes\":[";
    for (std::size_t i = 0; i < kGeneCount; ++i) {
        out << "{\"name\":\"" << kGeneSpecs[i].name << "\",\"lo\":" << kGeneSpecs[i].lo << ",\"hi\":" << kGeneSpecs[i].hi
            << '}' << (i + 1 < kGeneCount ? "," : "");
    }
    out << "],\"archetypes\":[";
    for (std::size_t a = 0; a < kArchetypeCount; ++a) {
        out << '"' << to_string(static_cast<Archetype>(a)) << '"' << (a + 1 < kArchetypeCount ? "," : "");
    }
    out << "],\"runs\":[";
    for (std::uint64_t s = 1; s <= seeds; ++s) {
        out << (s > 1 ? "," : "") << run_json(s, generations);
    }
    out << "]}\n";
    std::printf("wrote %s\n", path.c_str());
    return 0;
}
