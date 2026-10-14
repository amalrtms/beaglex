#include "beaglex/sim/population.hpp"
#include "beaglex/sim/simulation.hpp"

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <utility>
#include <vector>

using namespace beaglex;
using namespace beaglex::sim;

namespace {

// Runs a fixed list of actions at fixed times and records what it observes.
class ScriptedAgent final : public Agent {
public:
    using Action = std::function<void(AgentContext&)>;

    explicit ScriptedAgent(std::vector<std::pair<Time, Action>> script) : Agent(0), script_(std::move(script)) {}

    const char* kind() const noexcept override { return "scripted"; }

    void on_start(AgentContext& ctx) override {
        for (const auto& [time, action] : script_) {
            ctx.wake_after(time);
        }
    }

    void on_wakeup(AgentContext& ctx) override { script_[next_++].second(ctx); }

    void on_fill(AgentContext& ctx, const Fill& fill) override {
        Agent::on_fill(ctx, fill);
        fills.emplace_back(ctx.now(), fill);
    }

    std::vector<std::pair<Time, Fill>> fills;

private:
    std::vector<std::pair<Time, Action>> script_;
    std::size_t next_ = 0;
};

SimConfig short_config(std::uint64_t seed, Time duration) {
    SimConfig cfg;
    cfg.seed = seed;
    cfg.duration = duration;
    return cfg;
}

} // namespace

TEST(Simulation, LatencyDelaysOrdersFillsAndMarketData) {
    Simulation sim(short_config(1, kSecond));
    constexpr Time ms = kMillisecond;

    // A (5 ms away) posts a bid at t=0; it reaches the book at 5 ms.
    auto* a = new ScriptedAgent({{0, [](AgentContext& c) { c.submit(Side::Buy, OrderType::Limit, 10'000, 100); }}});
    // B (1 ms away) sends a market sell at t=0 (arrives at 1 ms: book empty,
    // nothing happens) and again at 10 ms (arrives at 11 ms: trades with A).
    auto* b = new ScriptedAgent({
        {0, [](AgentContext& c) { c.submit(Side::Sell, OrderType::Market, 0, 100); }},
        {10 * ms, [](AgentContext& c) { c.submit(Side::Sell, OrderType::Market, 0, 100); }},
    });
    // C (3 ms away) looks at the market three times.
    std::vector<MarketView> seen;
    auto* c = new ScriptedAgent({
        {7 * ms, [&](AgentContext& ctx) { seen.push_back(ctx.market()); }},  // as of 4 ms
        {9 * ms, [&](AgentContext& ctx) { seen.push_back(ctx.market()); }},  // as of 6 ms
        {15 * ms, [&](AgentContext& ctx) { seen.push_back(ctx.market()); }}, // as of 12 ms
    });

    sim.add_agent(std::unique_ptr<Agent>(a), 5 * ms);
    sim.add_agent(std::unique_ptr<Agent>(b), 1 * ms);
    sim.add_agent(std::unique_ptr<Agent>(c), 3 * ms);
    sim.run();

    ASSERT_EQ(sim.trades().size(), 1u);
    EXPECT_EQ(sim.trades()[0].time, 11 * ms);
    EXPECT_EQ(sim.trades()[0].price, 10'000);

    // Fills reach each side one latency after the trade.
    ASSERT_EQ(a->fills.size(), 1u);
    ASSERT_EQ(b->fills.size(), 1u);
    EXPECT_EQ(a->fills[0].first, 16 * ms);
    EXPECT_EQ(b->fills[0].first, 12 * ms);
    EXPECT_TRUE(a->fills[0].second.maker);
    EXPECT_FALSE(b->fills[0].second.maker);
    EXPECT_EQ(a->fills[0].second.fee, -200 * 100); // maker rebate
    EXPECT_EQ(b->fills[0].second.fee, 300 * 100);  // taker fee

    // C sees the market as it was 3 ms earlier.
    ASSERT_EQ(seen.size(), 3u);
    EXPECT_FALSE(seen[0].best_bid);           // A's bid not there yet at 4 ms
    EXPECT_EQ(seen[1].best_bid, 10'000);      // there at 6 ms
    EXPECT_FALSE(seen[2].best_bid);           // gone after the 11 ms trade
    EXPECT_EQ(seen[2].last_trade, 10'000);
}

TEST(Simulation, AgentsCannotCancelOthersOrders) {
    Simulation sim(short_config(1, kSecond));
    OrderId victim = 0;
    auto* a = new ScriptedAgent({{0, [&](AgentContext& c) { victim = c.submit(Side::Buy, OrderType::Limit, 10'000, 100); }}});
    auto* b = new ScriptedAgent({{5 * kMillisecond, [&](AgentContext& c) { c.cancel(victim); }}});
    auto* c = new ScriptedAgent({{20 * kMillisecond, [](AgentContext& ctx) { ctx.submit(Side::Sell, OrderType::Market, 0, 100); }}});
    sim.add_agent(std::unique_ptr<Agent>(a), kMillisecond);
    sim.add_agent(std::unique_ptr<Agent>(b), kMillisecond);
    sim.add_agent(std::unique_ptr<Agent>(c), kMillisecond);
    sim.run();

    ASSERT_EQ(sim.trades().size(), 1u); // A's order survived B's cancel
}

TEST(Simulation, PriceBandsStopExecutionsFarFromReference) {
    auto run = [](bool bands) {
        SimConfig cfg = short_config(1, 10 * kMinute);
        cfg.price_band.enabled = bands;
        cfg.price_band.width = 0.05;
        Simulation sim(cfg);
        // A market around 99.95 from the open; a far ask at 110.00 (10% away).
        sim.add_agent(std::unique_ptr<Agent>(new ScriptedAgent({{0, [](AgentContext& c) {
                          c.submit(Side::Buy, OrderType::Limit, 9'990, 100);
                          c.submit(Side::Sell, OrderType::Limit, 10'000, 100);
                          c.submit(Side::Sell, OrderType::Limit, 11'000, 100);
                      }}})),
                      kMillisecond);
        // Six minutes later a market buy for 300 tries to sweep both asks.
        sim.add_agent(std::unique_ptr<Agent>(new ScriptedAgent({{6 * kMinute, [](AgentContext& c) {
                          c.submit(Side::Buy, OrderType::Market, 0, 300);
                      }}})),
                      kMillisecond);
        sim.run();
        Price highest = 0;
        for (const auto& t : sim.trades()) {
            highest = std::max(highest, t.price);
        }
        return std::make_pair(sim.trades().size(), highest);
    };

    const auto [unbanded_trades, unbanded_high] = run(false);
    EXPECT_EQ(unbanded_trades, 2u);
    EXPECT_EQ(unbanded_high, 11'000);

    const auto [banded_trades, banded_high] = run(true);
    EXPECT_EQ(banded_trades, 1u); // 110.00 is beyond the 5% band around 99.95
    EXPECT_EQ(banded_high, 10'000);
}

TEST(Simulation, BreachingTheBandHaltsTrading) {
    SimConfig cfg = short_config(1, 20 * kMinute);
    cfg.price_band = {true, 0.05, 5 * kMinute, 5 * kMinute};
    Simulation sim(cfg);

    // Mid 99.95 at first; at 2 minutes the ask is pulled to 110.00 (mid 104.95).
    OrderId ask = 0;
    auto* maker = new ScriptedAgent({
        {0, [&](AgentContext& c) {
             c.submit(Side::Buy, OrderType::Limit, 9'990, 100);
             ask = c.submit(Side::Sell, OrderType::Limit, 10'000, 100);
         }},
        {2 * kMinute, [&](AgentContext& c) {
             c.cancel(ask);
             c.submit(Side::Sell, OrderType::Limit, 11'000, 300);
         }},
    });
    // At 6 minutes the band is centred on the 1-minute mid (99.95), so 110.00
    // is beyond it: the first buy halts trading for 5 minutes, the second is
    // rejected during the halt, and the third (12 minutes, band now centred on
    // 104.95) trades.
    auto* taker = new ScriptedAgent({
        {6 * kMinute, [](AgentContext& c) { c.submit(Side::Buy, OrderType::Market, 0, 100); }},
        {7 * kMinute, [](AgentContext& c) { c.submit(Side::Buy, OrderType::Limit, 11'000, 100); }},
        {12 * kMinute, [](AgentContext& c) { c.submit(Side::Buy, OrderType::Market, 0, 100); }},
    });
    sim.add_agent(std::unique_ptr<Agent>(maker), kMillisecond);
    sim.add_agent(std::unique_ptr<Agent>(taker), kMillisecond);
    sim.run();

    EXPECT_EQ(sim.halts(), 1u);
    EXPECT_EQ(sim.orders_rejected_in_halt(), 1u);
    ASSERT_EQ(sim.trades().size(), 1u);
    EXPECT_EQ(sim.trades()[0].price, 11'000);
    EXPECT_GE(sim.trades()[0].time, 12 * kMinute);
}

TEST(Simulation, SameSeedSameMarket) {
    auto run = [](std::uint64_t seed) {
        auto sim = std::make_unique<Simulation>(short_config(seed, 30 * kMinute));
        add_population(*sim, PopulationConfig{});
        sim->run();
        return sim;
    };
    const auto a = run(7);
    const auto b = run(7);
    const auto c = run(8);

    ASSERT_GT(a->trades().size(), 1000u);
    EXPECT_EQ(a->trades(), b->trades());
    EXPECT_EQ(a->events_processed(), b->events_processed());
    EXPECT_NE(a->trades(), c->trades());
}

TEST(Simulation, ConservesSharesAndCash) {
    Simulation sim(short_config(3, kHour));
    PopulationConfig pop;
    add_population(sim, pop);
    sim.run();

    Qty positions = 0, agent_volume = 0, traded = 0;
    Money cash = 0, fees = 0;
    for (AgentId id = 0; id < sim.agent_count(); ++id) {
        const Account& acc = sim.account(id);
        positions += acc.position;
        agent_volume += acc.volume;
        cash += acc.cash;
        fees += acc.fees;
    }
    for (const auto& t : sim.trades()) {
        traded += t.qty;
    }

    ASSERT_GT(traded, 0);
    EXPECT_EQ(positions, 0);                           // every share bought was sold by someone
    EXPECT_EQ(agent_volume, 2 * traded);               // each trade has two sides
    EXPECT_EQ(cash + sim.exchange_revenue(), 0);       // money only moves between agents and the exchange
    EXPECT_EQ(fees, sim.exchange_revenue());
}

TEST(Simulation, MarketMakersRespectPositionLimits) {
    Simulation sim(short_config(4, 2 * kHour));
    PopulationConfig pop;
    add_population(sim, pop);
    sim.run();

    // Fills already in flight can overshoot the limit by at most the quotes
    // resting on that side when the limit was reached.
    const Qty bound = pop.market_maker.max_position + 2 * pop.market_maker.quote_size;
    for (AgentId id = 0; id < pop.market_makers; ++id) {
        EXPECT_LE(std::abs(sim.account(id).position), bound);
    }
}

TEST(Simulation, MarketStaysTwoSided) {
    Simulation sim(short_config(5, kHour));
    add_population(sim, PopulationConfig{});
    sim.run();

    std::size_t two_sided = 0;
    for (const auto& s : sim.samples()) {
        two_sided += s.market.best_bid && s.market.best_ask ? 1 : 0;
    }
    // Allow the first moments of the day before market makers have quoted.
    EXPECT_GE(two_sided + 2, sim.samples().size());
}
