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

    // Attention: trading activity rises with recently observed volatility
    // (self-exciting order flow). Intensity = 1 + sensitivity * (sigma / reference - 1),
    // clamped to [0.5, max_intensity]. Zero sensitivity disables it.
    double activity_sensitivity = 10.0;
    double reference_volatility = 1.0; // ticks per sqrt(second)
    double max_intensity = 4.0;
    Time volatility_tau = 5 * kMinute;
};

class NoiseTrader final : public Agent {
public:
    NoiseTrader(const NoiseTraderConfig& cfg, std::uint64_t seed)
        : Agent(seed), cfg_(cfg), vol_(cfg.volatility_tau, cfg.reference_volatility) {}
    const char* kind() const noexcept override { return "noise"; }
    void on_start(AgentContext& ctx) override;
    void on_wakeup(AgentContext& ctx) override;
    void on_fill(AgentContext& ctx, const Fill& fill) override;

private:
    Time next_interval();

    NoiseTraderConfig cfg_;
    VolatilityEstimator vol_;
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

    // Liquidity withdrawal: when recent volatility runs above its longer-run
    // norm, the half-spread widens by `stress_spread` ticks per unit of excess
    // ratio (fast / slow - 1). Volatility is sampled once a second so
    // half-tick bounces between wake-ups are not mistaken for risk.
    // Zero disables it.
    double stress_spread = 8.0;
    Time fast_volatility_tau = 2 * kMinute;
    Time slow_volatility_tau = 30 * kMinute;
};

class MarketMaker final : public Agent {
public:
    MarketMaker(const MarketMakerConfig& cfg, std::uint64_t seed)
        : Agent(seed), cfg_(cfg), fast_vol_(cfg.fast_volatility_tau, 1.0), slow_vol_(cfg.slow_volatility_tau, 1.0) {}
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
    VolatilityEstimator fast_vol_;
    VolatilityEstimator slow_vol_;
    Time last_vol_sample_ = -kSecond;
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
    Time mean_interval = 30 * kSecond;
    double threshold = 0.001;     // acts on a 10 bp mispricing
    double noise_sigma = 0.0005;  // per-observation estimation error
    double bias_sigma = 0.00025;  // persistent per-agent bias
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
