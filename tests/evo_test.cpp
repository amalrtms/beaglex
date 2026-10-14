#include "beaglex/evo/evolution.hpp"

#include <gtest/gtest.h>

#include <memory>

using namespace beaglex;
using namespace beaglex::evo;

namespace {

bool within_bounds(const Genome& g) {
    for (std::size_t i = 0; i < kGeneCount; ++i) {
        if (g.genes[i] < kGeneSpecs[i].lo || g.genes[i] > kGeneSpecs[i].hi) {
            return false;
        }
    }
    return true;
}

EvolutionConfig tiny_config(std::uint64_t seed) {
    EvolutionConfig cfg;
    cfg.population = 6;
    cfg.days_per_generation = 2;
    cfg.elites = 2;
    cfg.seed = seed;
    cfg.market.duration = 20 * sim::kMinute;
    cfg.background.noise_traders = 30;
    cfg.background.momentum_traders = 5;
    cfg.background.value_traders = 10;
    return cfg;
}

} // namespace

TEST(Genome, RandomGenomesAreInBounds) {
    std::mt19937_64 rng(1);
    for (int i = 0; i < 1000; ++i) {
        EXPECT_TRUE(within_bounds(Genome::random(rng)));
    }
}

TEST(Genome, QuoteModeIsAFairCoinForRandomGenomes) {
    std::mt19937_64 rng(2);
    int quoting = 0;
    for (int i = 0; i < 10'000; ++i) {
        quoting += Genome::random(rng).quotes() ? 1 : 0;
    }
    EXPECT_NEAR(quoting / 10'000.0, 0.5, 0.02);
}

TEST(Genome, MutationStaysInBounds) {
    std::mt19937_64 rng(3);
    Genome g = Genome::random(rng);
    for (int i = 0; i < 1000; ++i) {
        mutate(g, 1.0, 0.5, rng); // every gene, large steps
        ASSERT_TRUE(within_bounds(g));
    }
}

TEST(Genome, CrossoverTakesEachGeneFromAParent) {
    std::mt19937_64 rng(4);
    const Genome a = Genome::random(rng);
    const Genome b = Genome::random(rng);
    for (int i = 0; i < 100; ++i) {
        const Genome c = crossover(a, b, rng);
        for (std::size_t k = 0; k < kGeneCount; ++k) {
            EXPECT_TRUE(c.genes[k] == a.genes[k] || c.genes[k] == b.genes[k]);
        }
    }
}

TEST(Genome, Classification) {
    Genome g{};
    g[kQuoteMode] = 0.9;
    EXPECT_EQ(classify(g), Archetype::MarketMaker);

    g[kQuoteMode] = 0.1;
    g[kMomentum] = 0.8;
    EXPECT_EQ(classify(g), Archetype::TrendFollower);
    g[kMomentum] = -0.8;
    EXPECT_EQ(classify(g), Archetype::MeanReverter);
    g[kImbalance] = 0.9;
    EXPECT_EQ(classify(g), Archetype::FlowFollower);
    g[kImbalance] = -0.9;
    EXPECT_EQ(classify(g), Archetype::FlowFader);
}

TEST(EvolvedTrader, ScoreIsWeightedSumOfSignals) {
    Genome g{};
    g[kMomentum] = 0.5;
    g[kReversion] = -1.0;
    g[kImbalance] = 0.25;
    g[kInventory] = -0.5;
    const EvolvedTrader t(g, EvolvedTraderConfig{}, 1);
    const Signals s{.momentum = 2.0, .reversion = 1.0, .imbalance = 0.4, .inventory = 0.5};
    EXPECT_DOUBLE_EQ(t.score(s), 0.5 * 2.0 - 1.0 * 1.0 + 0.25 * 0.4 - 0.5 * 0.5);
}

TEST(EvolvedTrader, NeverTradesBelowThreshold) {
    // Zero weights give a zero score, which never reaches a positive threshold.
    Genome g{};
    g[kThreshold] = 0.05;
    g[kLogInterval] = 0.0; // decide every second
    g[kLots] = 1.0;
    std::vector<Genome> genomes(3, g);

    EvolutionConfig cfg = tiny_config(1);
    Evolution evo(cfg);
    const Evaluation ev = evo.evaluate(genomes, 11, 1);
    EXPECT_DOUBLE_EQ(ev.days[0].evolved_volume_share, 0.0);
    for (double pnl : ev.days[0].pnl) {
        EXPECT_DOUBLE_EQ(pnl, 0.0);
    }
}

TEST(Evolution, SameSeedSameRun) {
    auto run = [](std::uint64_t seed) {
        Evolution evo(tiny_config(seed));
        std::vector<double> best;
        for (int g = 0; g < 3; ++g) {
            best.push_back(evo.step().best_fitness);
        }
        return std::make_pair(best, evo.population());
    };
    const auto [a_best, a_pop] = run(5);
    const auto [b_best, b_pop] = run(5);
    EXPECT_EQ(a_best, b_best);
    for (std::size_t i = 0; i < a_pop.size(); ++i) {
        EXPECT_EQ(a_pop[i].genes, b_pop[i].genes);
    }
}

TEST(Evolution, ElitesSurviveUnchanged) {
    Evolution evo(tiny_config(6));
    const std::vector<Genome> before = evo.population();
    const GenerationStats s = evo.step();
    // The generation's best genome is carried into the next population as-is.
    EXPECT_EQ(evo.population().front().genes, s.best.genes);
    bool found = false;
    for (const Genome& g : before) {
        found = found || g.genes == s.best.genes;
    }
    EXPECT_TRUE(found);
}
