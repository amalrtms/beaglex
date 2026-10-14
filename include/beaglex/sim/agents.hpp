#pragma once

// Agent archetypes from the agent-based market literature (e.g. Chiarella &
// Iori 2002; the fundamentalist / chartist / noise trader split), plus a
// liquidity-providing market maker. Each is deliberately simple: the point is
// to see what market behaviour emerges from their interaction.

#include "beaglex/sim/agent.hpp"

#include <optional>

namespace beaglex::sim {

// Uninformed flow: trades at random, mostly with limit orders near the mid.
struct NoiseTraderConfig {
    Time mean_interval = 20 * kSecond;
    double market_order_prob = 0.15;
    Qty lot = 100;
    int max_lots = 5;
    Price max_offset = 10; // limit prices up to this many ticks behind the mid...
    Price max_cross = 2;   // ...or up to this many through it
    Qty max_position = 1'000;
    Price fallback_price = 10'000; // used before the market has any prices
};

class NoiseTrader final : public Agent {
public:
    NoiseTrader(const NoiseTraderConfig& cfg, std::uint64_t seed) : Agent(seed), cfg_(cfg) {}
    const char* kind() const noexcept override { return "noise"; }
    void on_start(AgentContext& ctx) override;
    void on_wakeup(AgentContext& ctx) override;
    void on_fill(AgentContext& ctx, const Fill& fill) override;

private:
    NoiseTraderConfig cfg_;
    std::optional<OrderId> resting_;
};

// Liquidity provider: keeps a bid and an ask around the mid, skewed against
// its inventory so it sheds risk, and stops quoting a side at its position limit.
struct MarketMakerConfig {
    Time interval = 100 * kMillisecond;
    double half_spread = 1.5;     // ticks either side of the centre
    Qty quote_size = 200;
    Qty max_position = 5'000;
    double skew_per_share = 0.002; // centre shifts this many ticks per share held
    Price fallback_price = 10'000;
};

class MarketMaker final : public Agent {
public:
    MarketMaker(const MarketMakerConfig& cfg, std::uint64_t seed) : Agent(seed), cfg_(cfg) {}
    const char* kind() const noexcept override { return "market_maker"; }
    void on_start(AgentContext& ctx) override;
    void on_wakeup(AgentContext& ctx) override;
    void on_fill(AgentContext& ctx, const Fill& fill) override;

private:
    struct Quote {
        OrderId id;
        Price price;
    };
    void requote(AgentContext& ctx, std::optional<Quote>& quote, Side side, Price price, bool want);

    MarketMakerConfig cfg_;
    std::optional<Quote> bid_;
    std::optional<Quote> ask_;
};

// Chartist: follows trends signalled by a fast moving average crossing a slow one.
struct MomentumTraderConfig {
    Time mean_interval = 60 * kSecond;
    Time fast_tau = 2 * kMinute;  // EMA time constants
    Time slow_tau = 15 * kMinute;
    double threshold = 0.0005;    // 5 bp gap between the averages
    Time warmup = 15 * kMinute;   // no trading until the slow average means something
    Qty lot = 100;
    int max_lots = 3;
    Qty max_position = 1'000;
    Price max_slippage = 2; // IOC limit this many ticks through the touch
};

class MomentumTrader final : public Agent {
public:
    MomentumTrader(const MomentumTraderConfig& cfg, std::uint64_t seed) : Agent(seed), cfg_(cfg) {}
    const char* kind() const noexcept override { return "momentum"; }
    void on_start(AgentContext& ctx) override;
    void on_wakeup(AgentContext& ctx) override;

private:
    MomentumTraderConfig cfg_;
    bool initialised_ = false;
    double fast_ = 0;
    double slow_ = 0;
    Time last_update_ = 0;
};

// Fundamentalist: has a noisy, privately biased estimate of the true value
// and trades when the market price strays far enough from it.
struct ValueTraderConfig {
    Time mean_interval = 90 * kSecond;
    double threshold = 0.002;   // acts on a 20 bp mispricing
    double noise_sigma = 0.002; // per-observation estimation error
    double bias_sigma = 0.001;  // persistent per-agent bias
    Qty lot = 100;
    int max_lots = 10;
    Qty max_position = 4'000; // institutional size: much larger than noise flow
};

class ValueTrader final : public Agent {
public:
    ValueTrader(const ValueTraderConfig& cfg, std::uint64_t seed);
    const char* kind() const noexcept override { return "value"; }
    void on_start(AgentContext& ctx) override;
    void on_wakeup(AgentContext& ctx) override;

private:
    ValueTraderConfig cfg_;
    double bias_;
};

} // namespace beaglex::sim
