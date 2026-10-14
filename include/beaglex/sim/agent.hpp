#pragma once

#include "beaglex/sim/types.hpp"

#include <algorithm>
#include <cstdint>
#include <random>

namespace beaglex::sim {

class Simulation;

// An agent's only window onto the world. Everything it sees is delayed by its
// latency and everything it sends reaches the exchange one latency later.
class AgentContext {
public:
    AgentContext(Simulation& sim, AgentId id) noexcept : sim_(sim), id_(id) {}

    AgentId id() const noexcept { return id_; }
    Time now() const noexcept;
    Time latency() const noexcept;

    // Top of book as of now - latency.
    MarketView market() const;

    // Fundamental value (in ticks) as of now - latency. Only agents that model
    // informed traders should call this.
    double fundamental() const;

    // Sends an order; it arrives at the exchange after the agent's latency.
    // Returns the id the exchange will use for it.
    OrderId submit(Side side, OrderType type, Price price, Qty qty);

    // Requests cancellation; harmless if the order has already filled.
    void cancel(OrderId id);

    // Schedules the agent's next on_wakeup.
    void wake_after(Time delay);

private:
    Simulation& sim_;
    AgentId id_;
};

class Agent {
public:
    explicit Agent(std::uint64_t seed) : rng_(seed) {}
    virtual ~Agent() = default;

    virtual const char* kind() const noexcept = 0;
    virtual void on_start(AgentContext& ctx) = 0;
    virtual void on_wakeup(AgentContext& ctx) = 0;

    // Default bookkeeping keeps the agent's own (delayed) view of its position.
    virtual void on_fill(AgentContext& /*ctx*/, const Fill& fill) {
        position_ += fill.side == Side::Buy ? fill.qty : -fill.qty;
    }

    Qty position() const noexcept { return position_; }

protected:
    double uniform() { return std::uniform_real_distribution<double>(0.0, 1.0)(rng_); }
    std::int64_t uniform_int(std::int64_t lo, std::int64_t hi) {
        return std::uniform_int_distribution<std::int64_t>(lo, hi)(rng_);
    }
    double normal(double mean, double sd) { return std::normal_distribution<double>(mean, sd)(rng_); }
    Time exponential(Time mean) {
        const double t = std::exponential_distribution<double>(1.0 / static_cast<double>(mean))(rng_);
        return std::max<Time>(1, static_cast<Time>(t));
    }

    std::mt19937_64 rng_;
    Qty position_ = 0;
};

} // namespace beaglex::sim
