// JavaScript bindings for the browser playground. A Market wraps one
// Simulation that the page advances a little every animation frame, pulling
// new samples and trades incrementally and poking it with live interventions.

#include "beaglex/evo/evolved_trader.hpp"
#include "beaglex/evo/presets.hpp"
#include "beaglex/sim/population.hpp"
#include "beaglex/sim/simulation.hpp"

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace beaglex;
using emscripten::val;

namespace {

std::uint64_t mix(std::uint64_t x) {
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

double price_or_nan(const std::optional<Price>& p) {
    return p ? static_cast<double>(*p) : kNaN;
}

// Copies into a fresh Float64Array, so JS never holds a view into wasm memory
// that a later allocation could invalidate.
val to_float64_array(const std::vector<double>& v) {
    return val::global("Float64Array").new_(emscripten::typed_memory_view(v.size(), v.data()));
}

} // namespace

class Market {
public:
    Market(unsigned seed, int market_makers, int noise_traders, int momentum_traders, int value_traders)
        : seed_(seed), algo_rng_(mix(seed)) {
        sim::SimConfig cfg;
        cfg.seed = seed;
        sim_ = std::make_unique<sim::Simulation>(cfg);

        sim::PopulationConfig pop;
        pop.market_makers = static_cast<std::size_t>(market_makers);
        pop.noise_traders = static_cast<std::size_t>(noise_traders);
        pop.momentum_traders = static_cast<std::size_t>(momentum_traders);
        pop.value_traders = static_cast<std::size_t>(value_traders);
        sim::add_population(*sim_, pop);
        for (sim::AgentId id = 0; id < sim_->agent_count(); ++id) {
            groups_.push_back(sim_->agent(id).kind());
        }
        sim_->start();
    }

    // Advances the simulated clock by `seconds`; false once the day is over.
    bool advance(double seconds) {
        const auto target = sim_->now() + static_cast<sim::Time>(seconds * static_cast<double>(sim::kSecond));
        return sim_->advance_to(target);
    }

    double time() const { return static_cast<double>(sim_->now()) / sim::kSecond; }
    double duration() const { return static_cast<double>(sim_->config().duration) / sim::kSecond; }
    double events() const { return static_cast<double>(sim_->events_processed()); }
    int sampleCount() const { return static_cast<int>(sim_->samples().size()); }
    int tradeCount() const { return static_cast<int>(sim_->trades().size()); }
    bool halted() const { return sim_->halted(); }
    int halts() const { return static_cast<int>(sim_->halts()); }

    // Samples from index `from` on, flattened: [seconds, bid, ask, last, fundamental] each,
    // prices in ticks (cents) and NaN where the book side is empty.
    val samplesSince(int from) const {
        const auto& s = sim_->samples();
        std::vector<double> out;
        out.reserve((s.size() - std::min<std::size_t>(from, s.size())) * 5);
        for (std::size_t i = static_cast<std::size_t>(from); i < s.size(); ++i) {
            out.push_back(static_cast<double>(s[i].time) / sim::kSecond);
            out.push_back(price_or_nan(s[i].market.best_bid));
            out.push_back(price_or_nan(s[i].market.best_ask));
            out.push_back(price_or_nan(s[i].market.last_trade));
            out.push_back(s[i].fundamental);
        }
        return to_float64_array(out);
    }

    // Trades from index `from` on, flattened: [seconds, price, qty, +1 buy / -1 sell taker].
    val tradesSince(int from) const {
        const auto& t = sim_->trades();
        std::vector<double> out;
        out.reserve((t.size() - std::min<std::size_t>(from, t.size())) * 4);
        for (std::size_t i = static_cast<std::size_t>(from); i < t.size(); ++i) {
            out.push_back(static_cast<double>(t[i].time) / sim::kSecond);
            out.push_back(static_cast<double>(t[i].price));
            out.push_back(static_cast<double>(t[i].qty));
            out.push_back(t[i].taker_side == Side::Buy ? 1.0 : -1.0);
        }
        return to_float64_array(out);
    }

    // The live book, best levels first: [price, qty, orders] per level.
    val depth(Side side, int max_levels) const {
        std::vector<double> out;
        for (const auto& l : sim_->book().levels(side, static_cast<std::size_t>(max_levels))) {
            out.push_back(static_cast<double>(l.price));
            out.push_back(static_cast<double>(l.qty));
            out.push_back(static_cast<double>(l.orders));
        }
        return to_float64_array(out);
    }
    val bids(int max_levels) const { return depth(Side::Buy, max_levels); }
    val asks(int max_levels) const { return depth(Side::Sell, max_levels); }

    // Profit and activity per agent group, marked to the last price.
    val groupStats() const {
        const auto& samples = sim_->samples();
        double mark = sim_->config().fundamental.initial;
        if (!samples.empty() && samples.back().market.last_trade) {
            mark = static_cast<double>(*samples.back().market.last_trade);
        }
        struct Totals {
            int count = 0;
            double pnl = 0, volume = 0, position = 0;
        };
        std::map<std::string, Totals> totals;
        for (sim::AgentId id = 0; id < sim_->agent_count(); ++id) {
            const sim::Account& a = sim_->account(id);
            Totals& t = totals[groups_[id]];
            ++t.count;
            t.pnl += sim::to_dollars(a.mark_to_market(mark));
            t.volume += static_cast<double>(a.volume);
            t.position += static_cast<double>(a.position);
        }
        val out = val::array();
        for (const auto& [name, t] : totals) {
            val row = val::object();
            row.set("group", name);
            row.set("count", t.count);
            row.set("pnl", t.pnl);
            row.set("volume", t.volume);
            row.set("position", t.position);
            out.call<void>("push", row);
        }
        return out;
    }

    // Phase 6's trend-chasing algorithm, either n identical copies (herding)
    // or n variants with their own signal weights, thresholds and speeds.
    void addMomentumAlgos(int n, bool diverse) {
        evo::EvolvedTraderConfig trader;
        trader.fallback_price = sim_->config().fundamental.initial;
        auto u = [&](double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(algo_rng_); };
        for (int i = 0; i < n; ++i) {
            evo::Genome g = evo::presets::momentum_algo();
            if (diverse) {
                g[evo::kMomentum] = u(0.5, 1.0);
                g[evo::kReversion] = u(0.5, 1.0);
                g[evo::kImbalance] = u(0.0, 0.5);
                g[evo::kThreshold] = u(0.3, 1.5);
                g[evo::kLogInterval] = u(0.0, 1.5);
            }
            sim_->add_agent(std::make_unique<evo::EvolvedTrader>(g, trader, mix(seed_ * 1'000 + algos_++)),
                            5 * sim::kMillisecond);
            groups_.push_back(diverse ? "algo_diverse" : "algo_identical");
        }
    }

    // Adds n traders running one genome (ten numbers, in kGeneSpecs order),
    // reported under `group` in groupStats.
    void addStrategy(const val& genes, int n, const std::string& group) {
        const auto values = emscripten::vecFromJSArray<double>(genes);
        evo::Genome g;
        for (std::size_t i = 0; i < evo::kGeneCount && i < values.size(); ++i) {
            g.genes[i] = values[i];
        }
        g.clamp();
        evo::EvolvedTraderConfig trader;
        trader.fallback_price = sim_->config().fundamental.initial;
        for (int i = 0; i < n; ++i) {
            sim_->add_agent(std::make_unique<evo::EvolvedTrader>(g, trader, mix(seed_ * 1'000 + algos_++)),
                            5 * sim::kMillisecond);
            groups_.push_back(group);
        }
    }

    // Moves the hidden fundamental value by `percent` from now on.
    void newsShock(double percent) { sim_->shock_fundamental(std::log1p(percent / 100.0)); }

    // 0 = off, 1 = 5% price bands, 2 = 5% bands with 5-minute trading halts.
    void setCircuitBreaker(int mode) {
        sim::PriceBandConfig band;
        band.enabled = mode > 0;
        band.width = 0.05;
        band.reference_lag = 5 * sim::kMinute;
        band.halt_duration = mode == 2 ? 5 * sim::kMinute : 0;
        sim_->set_price_band(band);
    }

private:
    std::uint64_t seed_;
    std::mt19937_64 algo_rng_;
    std::uint64_t algos_ = 0;
    std::unique_ptr<sim::Simulation> sim_;
    std::vector<std::string> groups_;
};

// Named strategies from presets.hpp, as plain gene arrays.
val presetGenome(const std::string& name) {
    const evo::Genome g = name == "evolved_market_maker" ? evo::presets::evolved_market_maker()
                                                         : evo::presets::momentum_algo();
    val out = val::array();
    for (double v : g.genes) {
        out.call<void>("push", v);
    }
    return out;
}

EMSCRIPTEN_BINDINGS(beaglex) {
    emscripten::function("presetGenome", &presetGenome);
    emscripten::class_<Market>("Market")
        .constructor<unsigned, int, int, int, int>()
        .function("advance", &Market::advance)
        .function("time", &Market::time)
        .function("duration", &Market::duration)
        .function("events", &Market::events)
        .function("sampleCount", &Market::sampleCount)
        .function("tradeCount", &Market::tradeCount)
        .function("halted", &Market::halted)
        .function("halts", &Market::halts)
        .function("samplesSince", &Market::samplesSince)
        .function("tradesSince", &Market::tradesSince)
        .function("bids", &Market::bids)
        .function("asks", &Market::asks)
        .function("groupStats", &Market::groupStats)
        .function("addMomentumAlgos", &Market::addMomentumAlgos)
        .function("addStrategy", &Market::addStrategy)
        .function("newsShock", &Market::newsShock)
        .function("setCircuitBreaker", &Market::setCircuitBreaker);
}
