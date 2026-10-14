#pragma once

#include "beaglex/evo/genome.hpp"
#include "beaglex/sim/agent.hpp"

#include <optional>

namespace beaglex::evo {

struct EvolvedTraderConfig {
    Qty lot = 100;
    Qty max_position = 500;
    sim::Time fast_tau = sim::kMinute;      // moving-average time constants for the signals
    sim::Time slow_tau = 10 * sim::kMinute;
    Price slippage = 1;                     // aggressive orders reach this many ticks through the touch
    Price fallback_price = 10'000;
};

// Signals in comparable units, each roughly in [-3, 3]. Exposed for testing.
struct Signals {
    double momentum = 0;  // (fast MA - slow MA) / slow MA, in 10 bp units
    double reversion = 0; // (price - fast MA) / price, in 10 bp units
    double imbalance = 0; // (bid size - ask size) / (bid size + ask size)
    double inventory = 0; // position / max position
};

// Executes a Genome. Uninformed: it sees only delayed market data and its own
// position, never the fundamental value.
//
// Directional mode: score = weights . signals. Above +threshold it buys, below
// -threshold it sells, taking liquidity with probability `aggression` and
// otherwise joining the best price with one working order.
//
// Quoting mode (quote_mode > 0.5): it keeps a bid and an ask half_spread ticks
// either side of the price, shifted by the score, like a market maker whose
// quotes lean on its signals.
class EvolvedTrader final : public sim::Agent {
public:
    EvolvedTrader(const Genome& genome, const EvolvedTraderConfig& cfg, std::uint64_t seed)
        : Agent(seed), genome_(genome), cfg_(cfg) {}

    const char* kind() const noexcept override { return "evolved"; }
    void on_start(sim::AgentContext& ctx) override;
    void on_wakeup(sim::AgentContext& ctx) override;
    void on_fill(sim::AgentContext& ctx, const sim::Fill& fill) override;

    const Genome& genome() const noexcept { return genome_; }
    double score(const Signals& s) const noexcept;

private:
    struct Quote {
        OrderId id;
        Price price;
    };

    sim::Time next_interval();
    void trade_directionally(sim::AgentContext& ctx, const sim::MarketView& view, double score);
    void quote(sim::AgentContext& ctx, const sim::MarketView& view, double ref, double score);
    void requote(sim::AgentContext& ctx, std::optional<Quote>& q, Side side, Price price, bool want);

    Genome genome_;
    EvolvedTraderConfig cfg_;
    bool initialised_ = false;
    double fast_ = 0;
    double slow_ = 0;
    sim::Time last_update_ = 0;
    std::optional<OrderId> working_;
    std::optional<Quote> bid_;
    std::optional<Quote> ask_;
};

} // namespace beaglex::evo
