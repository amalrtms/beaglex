#include "beaglex/evo/evolution.hpp"

#include "beaglex/analysis/stylized_facts.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <numeric>
#include <thread>

namespace beaglex::evo {

namespace {

std::uint64_t mix(std::uint64_t x) {
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

} // namespace

double Evaluation::pooled_kurtosis() const {
    analysis::Series pooled;
    for (const auto& d : days) {
        pooled.insert(pooled.end(), d.minute_returns.begin(), d.minute_returns.end());
    }
    return analysis::excess_kurtosis(pooled);
}

double Evaluation::day_mean(double DayResult::*field) const {
    double s = 0;
    for (const auto& d : days) {
        s += d.*field;
    }
    return s / static_cast<double>(days.size());
}

Evolution::Evolution(const EvolutionConfig& config) : config_(config), rng_(mix(config.seed)) {
    population_.reserve(config_.population);
    for (std::size_t i = 0; i < config_.population; ++i) {
        population_.push_back(Genome::random(rng_));
    }
}

DayResult Evolution::simulate_day(const std::vector<Genome>& genomes, std::uint64_t seed) const {
    sim::SimConfig market = config_.market;
    market.seed = seed;
    sim::Simulation s(market);
    sim::add_population(s, config_.background);

    EvolvedTraderConfig trader = config_.trader;
    trader.fallback_price = market.fundamental.initial;
    const auto first = static_cast<sim::AgentId>(s.agent_count());
    for (std::size_t i = 0; i < genomes.size(); ++i) {
        s.add_agent(std::make_unique<EvolvedTrader>(genomes[i], trader, mix(seed * 1'000 + i)), config_.evolved_latency);
    }
    const auto first_fixed = static_cast<sim::AgentId>(s.agent_count());
    for (std::size_t i = 0; i < config_.fixed_strategies.size(); ++i) {
        s.add_agent(std::make_unique<EvolvedTrader>(config_.fixed_strategies[i], trader, mix(seed * 1'000 + 500 + i)),
                    config_.evolved_latency);
    }
    s.run();

    const analysis::DayData day = analysis::extract_day(s);
    const double close = day.price.back();

    // Liquidating the closing position costs about half the spread: one tick per share.
    auto net_pnl = [&](sim::AgentId id) {
        const sim::Account& a = s.account(id);
        return sim::to_dollars(a.mark_to_market(close) - std::abs(a.position) * sim::kMoneyPerTick);
    };

    DayResult r;
    r.pnl.resize(genomes.size());
    Qty evolved_volume = 0;
    for (std::size_t i = 0; i < genomes.size(); ++i) {
        const auto id = first + static_cast<sim::AgentId>(i);
        r.pnl[i] = net_pnl(id);
        evolved_volume += s.account(id).volume;
    }
    for (std::size_t i = 0; i < config_.fixed_strategies.size(); ++i) {
        r.fixed_pnl += net_pnl(first_fixed + static_cast<sim::AgentId>(i)) /
                       static_cast<double>(config_.fixed_strategies.size());
    }

    Qty traded = 0;
    for (const auto& t : s.trades()) {
        traded += t.qty;
    }
    r.evolved_volume_share = traded > 0 ? static_cast<double>(evolved_volume) / (2.0 * static_cast<double>(traded)) : 0.0;
    for (std::size_t t = 0; t < day.price.size(); ++t) {
        const double deviation = std::abs(day.price[t] / day.fundamental[t] - 1.0);
        r.max_deviation_pct = std::max(r.max_deviation_pct, deviation * 100.0);
        r.mean_deviation_bp += deviation * 1e4 / static_cast<double>(day.price.size());
    }

    r.halts = static_cast<double>(s.halts());
    r.minute_returns = analysis::log_returns(day.price, 60);
    r.return_sd_bp = analysis::stddev(r.minute_returns) * 1e4;
    r.abs_return_acf1 = analysis::autocorrelation({analysis::absolute(r.minute_returns)}, 1);

    double spread = 0;
    std::size_t n = 0;
    for (const auto& smp : s.samples()) {
        if (smp.market.best_bid && smp.market.best_ask) {
            spread += static_cast<double>(*smp.market.best_ask - *smp.market.best_bid);
            ++n;
        }
    }
    r.avg_spread_ticks = n > 0 ? spread / static_cast<double>(n) : 0.0;
    return r;
}

Evaluation Evolution::evaluate(const std::vector<Genome>& genomes, std::uint64_t first_seed, std::size_t days) const {
    Evaluation ev;
    ev.days.resize(days);
    std::vector<std::thread> workers;
    workers.reserve(days);
    for (std::size_t d = 0; d < days; ++d) {
        workers.emplace_back([&, d] { ev.days[d] = simulate_day(genomes, first_seed + d); });
    }
    for (auto& w : workers) {
        w.join();
    }

    ev.fitness.resize(genomes.size());
    for (std::size_t i = 0; i < genomes.size(); ++i) {
        double sum = 0, sum_sq = 0;
        for (const auto& day : ev.days) {
            sum += day.pnl[i];
            sum_sq += day.pnl[i] * day.pnl[i];
        }
        const auto n = static_cast<double>(days);
        const double mean = sum / n;
        const double var = days > 1 ? std::max(0.0, (sum_sq - n * mean * mean) / (n - 1)) : 0.0;
        ev.fitness[i] = mean - config_.risk_aversion * std::sqrt(var);
    }
    return ev;
}

const Genome& Evolution::tournament_pick(const std::vector<double>& fitness) {
    std::uniform_int_distribution<std::size_t> pick(0, population_.size() - 1);
    std::size_t best = pick(rng_);
    for (std::size_t k = 1; k < config_.tournament; ++k) {
        const std::size_t c = pick(rng_);
        if (fitness[c] > fitness[best]) {
            best = c;
        }
    }
    return population_[best];
}

GenerationStats Evolution::step() {
    // Fresh market days every generation, so strategies cannot memorise one day.
    const std::uint64_t first_seed = config_.seed * 1'000'003 + generation_ * config_.days_per_generation;
    const Evaluation ev = evaluate(population_, first_seed, config_.days_per_generation);

    std::vector<std::size_t> order(population_.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return ev.fitness[a] > ev.fitness[b]; });

    GenerationStats st;
    st.generation = generation_;
    st.best_fitness = ev.fitness[order.front()];
    st.median_fitness = ev.fitness[order[order.size() / 2]];
    st.mean_fitness = std::accumulate(ev.fitness.begin(), ev.fitness.end(), 0.0) / static_cast<double>(ev.fitness.size());
    st.best = population_[order.front()];
    for (const Genome& g : population_) {
        for (std::size_t i = 0; i < kGeneCount; ++i) {
            st.mean_genes[i] += g.genes[i] / static_cast<double>(population_.size());
        }
        ++st.archetypes[static_cast<std::size_t>(classify(g))];
    }
    st.return_sd_bp = ev.day_mean(&DayResult::return_sd_bp);
    st.excess_kurtosis = ev.pooled_kurtosis();
    st.abs_return_acf1 = ev.day_mean(&DayResult::abs_return_acf1);
    st.avg_spread_ticks = ev.day_mean(&DayResult::avg_spread_ticks);
    st.evolved_volume_share = ev.day_mean(&DayResult::evolved_volume_share);
    st.max_deviation_pct = ev.day_mean(&DayResult::max_deviation_pct);

    // Breed the next generation: elites survive, the rest are offspring of
    // tournament winners.
    std::vector<Genome> next;
    next.reserve(population_.size());
    for (std::size_t i = 0; i < std::min(config_.elites, order.size()); ++i) {
        next.push_back(population_[order[i]]);
    }
    std::bernoulli_distribution do_crossover(config_.crossover_rate);
    while (next.size() < population_.size()) {
        Genome child = tournament_pick(ev.fitness);
        if (do_crossover(rng_)) {
            child = crossover(child, tournament_pick(ev.fitness), rng_);
        }
        mutate(child, config_.mutation_rate, config_.mutation_scale, rng_);
        next.push_back(child);
    }
    population_ = std::move(next);
    ++generation_;
    return st;
}

} // namespace beaglex::evo
