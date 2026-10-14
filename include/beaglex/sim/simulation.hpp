#pragma once

#include "beaglex/order_book.hpp"
#include "beaglex/sim/agent.hpp"
#include "beaglex/sim/types.hpp"

#include <memory>
#include <queue>
#include <vector>

namespace beaglex::sim {

struct FeeSchedule {
    Money taker_fee = 300;    // per share: 0.30 ticks, i.e. 0.30 cents
    Money maker_rebate = 200; // per share, paid to the resting side
};

// The hidden "true value" of the asset: a geometric random walk with
// occasional news jumps. Price is free to deviate from it; whether it tracks
// it depends on the agents.
struct FundamentalConfig {
    Price initial = 10'000;              // $100.00
    double volatility_per_second = 0.5e-4; // ~0.8% over a 6.5 hour day
    double jumps_per_hour = 0.5;
    double jump_sigma = 0.004;           // 0.4% news moves
};

// Exchange price bands, a simplified "limit up-limit down" rule. Incoming
// orders cannot execute more than `width` away from the mid price of
// `reference_lag` ago: buy prices are capped at the upper band, sell prices
// floored at the lower one, and market orders become IOC orders at the band.
//
// With a positive `halt_duration`, an order that would trade against a market
// already beyond the band instead triggers a trading pause: for that long,
// new orders are rejected (cancels still work), as in the real rule's 5-minute
// pauses. The real rule's 15-second "limit state" and reopening auction are
// not modelled.
struct PriceBandConfig {
    bool enabled = false;
    double width = 0.05;
    Time reference_lag = 5 * kMinute;
    Time halt_duration = 0;
};

struct SimConfig {
    std::uint64_t seed = 1;
    PriceBandConfig price_band;
    Time duration = 6 * kHour + 30 * kMinute; // one trading day
    Time sample_interval = kSecond;
    FeeSchedule fees;
    FundamentalConfig fundamental;
};

struct TradeRecord {
    Time time = 0;
    Price price = 0;
    Qty qty = 0;
    Side taker_side = Side::Buy;
    AgentId maker = 0;
    AgentId taker = 0;

    bool operator==(const TradeRecord&) const = default;
};

// The true (undelayed) market state at a sample time.
struct Sample {
    Time time = 0;
    MarketView market;
    double fundamental = 0;
    std::size_t resting_orders = 0;
};

// Discrete-event market simulation: one exchange, many agents, a simulated
// nanosecond clock. Events are processed in (time, sequence) order, so a run
// is fully determined by its seed and agent population.
class Simulation {
public:
    explicit Simulation(const SimConfig& config);

    Simulation(const Simulation&) = delete;
    Simulation& operator=(const Simulation&) = delete;

    // Adds an agent with a one-way latency to the exchange. Orders, fills and
    // market data are all delayed by it. Agents added after start() join the
    // market immediately.
    AgentId add_agent(std::unique_ptr<Agent> agent, Time latency);

    // Runs the whole day: start() then advance_to(duration).
    void run();

    // Stepwise running, for driving the simulation interactively. start()
    // must be called once before advance_to(), which processes every event up
    // to and including time t and returns false once the day is over.
    void start();
    bool advance_to(Time t);
    bool started() const noexcept { return started_; }
    Time now() const noexcept { return now_; }

    // Live interventions. A news shock moves the fundamental by `log_return`
    // from the current second onwards; agents learn of it through their
    // delayed view, like any other news.
    void shock_fundamental(double log_return);
    void set_price_band(const PriceBandConfig& band) { config_.price_band = band; }
    bool halted() const noexcept { return now_ < halted_until_; }

    const SimConfig& config() const noexcept { return config_; }
    std::size_t agent_count() const noexcept { return agents_.size(); }
    const Agent& agent(AgentId id) const { return *agents_[id].agent; }
    const Account& account(AgentId id) const { return agents_[id].account; }
    Time latency(AgentId id) const { return agents_[id].latency; }

    const OrderBook& book() const noexcept { return book_; }
    const std::vector<TradeRecord>& trades() const noexcept { return trades_; }
    const std::vector<Sample>& samples() const noexcept { return samples_; }
    Money exchange_revenue() const noexcept { return exchange_revenue_; }
    std::size_t events_processed() const noexcept { return events_; }
    std::size_t band_adjusted_orders() const noexcept { return band_adjusted_; }
    std::size_t band_adjusted_orders(AgentId id) const { return agents_[id].band_adjusted; }
    std::size_t halts() const noexcept { return halts_; }
    std::size_t orders_rejected_in_halt() const noexcept { return orders_rejected_in_halt_; }

    double fundamental_at(Time t) const;

private:
    friend class AgentContext;

    enum class EventKind : std::uint8_t { Wakeup, OrderArrival, CancelArrival, FillDelivery, Sample };

    struct Event {
        Time time = 0;
        std::uint64_t seq = 0; // tie-break: earlier-scheduled events go first
        EventKind kind = EventKind::Wakeup;
        AgentId agent = 0;
        NewOrder order;
        Fill fill;
    };

    struct EventLater {
        bool operator()(const Event& a, const Event& b) const noexcept {
            return a.time != b.time ? a.time > b.time : a.seq > b.seq;
        }
    };

    struct Slot {
        std::unique_ptr<Agent> agent;
        Time latency = 0;
        Account account;
        std::size_t band_adjusted = 0;
    };

    void schedule(Event e);
    void process(const Event& e);
    void on_order_arrival(const Event& e);
    struct BandResult {
        NewOrder order;
        bool breach;
    };
    BandResult apply_price_band(NewOrder order, AgentId agent);
    void on_cancel_arrival(const Event& e);
    void settle(const Trade& t, Qty maker_leaves, Qty taker_leaves);
    void record_top_of_book();
    MarketView current_view() const;
    MarketView view_at(Time t) const;

    SimConfig config_;
    OrderBook book_;
    std::vector<Slot> agents_;
    std::priority_queue<Event, std::vector<Event>, EventLater> queue_;
    std::uint64_t next_seq_ = 0;
    OrderId next_order_id_ = 1;
    Time now_ = 0;
    bool started_ = false;
    std::optional<Price> last_trade_;
    Money exchange_revenue_ = 0;
    std::size_t events_ = 0;
    std::size_t band_adjusted_ = 0;
    Time halted_until_ = 0;
    std::size_t halts_ = 0;
    std::size_t orders_rejected_in_halt_ = 0;

    std::vector<double> fundamental_;    // one value per simulated second
    std::vector<MarketView> tob_history_; // every change of top of book, in time order
    std::vector<TradeRecord> trades_;
    std::vector<Sample> samples_;
};

} // namespace beaglex::sim
