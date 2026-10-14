#include "beaglex/sim/simulation.hpp"

#include <algorithm>
#include <cmath>
#include <random>

namespace beaglex::sim {

Simulation::Simulation(const SimConfig& config) : config_(config) {
    book_.reserve(1 << 16);

    // Pre-generate the fundamental path at one-second resolution so it is
    // independent of how many events the agents create.
    const auto& f = config_.fundamental;
    const auto seconds = static_cast<std::size_t>(config_.duration / kSecond) + 2;
    std::mt19937_64 rng(config_.seed ^ 0x9e3779b97f4a7c15ULL);
    std::normal_distribution<double> gauss(0.0, 1.0);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    const double jump_p = f.jumps_per_hour / 3600.0;

    fundamental_.resize(seconds);
    double log_value = std::log(static_cast<double>(f.initial));
    for (auto& v : fundamental_) {
        v = std::exp(log_value);
        log_value += f.volatility_per_second * gauss(rng);
        if (unit(rng) < jump_p) {
            log_value += f.jump_sigma * gauss(rng);
        }
    }

    tob_history_.push_back(MarketView{});
}

AgentId Simulation::add_agent(std::unique_ptr<Agent> agent, Time latency) {
    agents_.push_back(Slot{std::move(agent), latency, {}});
    return static_cast<AgentId>(agents_.size() - 1);
}

double Simulation::fundamental_at(Time t) const {
    const auto i = static_cast<std::size_t>(std::clamp<Time>(t / kSecond, 0, static_cast<Time>(fundamental_.size()) - 1));
    return fundamental_[i];
}

void Simulation::schedule(Event e) {
    e.seq = next_seq_++;
    queue_.push(std::move(e));
}

void Simulation::run() {
    for (AgentId id = 0; id < agents_.size(); ++id) {
        AgentContext ctx(*this, id);
        agents_[id].agent->on_start(ctx);
    }
    schedule(Event{.time = 0, .kind = EventKind::Sample});

    while (!queue_.empty()) {
        Event e = queue_.top();
        if (e.time > config_.duration) {
            break;
        }
        queue_.pop();
        now_ = e.time;
        ++events_;
        process(e);
    }
}

void Simulation::process(const Event& e) {
    switch (e.kind) {
    case EventKind::Wakeup: {
        AgentContext ctx(*this, e.agent);
        agents_[e.agent].agent->on_wakeup(ctx);
        break;
    }
    case EventKind::OrderArrival:
        on_order_arrival(e);
        break;
    case EventKind::CancelArrival:
        on_cancel_arrival(e);
        break;
    case EventKind::FillDelivery: {
        AgentContext ctx(*this, e.agent);
        agents_[e.agent].agent->on_fill(ctx, e.fill);
        break;
    }
    case EventKind::Sample:
        samples_.push_back(Sample{now_, current_view(), fundamental_at(now_), book_.order_count()});
        schedule(Event{.time = now_ + config_.sample_interval, .kind = EventKind::Sample});
        break;
    }
}

Simulation::BandResult Simulation::apply_price_band(NewOrder order, AgentId agent) {
    const PriceBandConfig& band = config_.price_band;
    if (!band.enabled || now_ < band.reference_lag) {
        return {order, false};
    }
    const auto reference = view_at(now_ - band.reference_lag).mid();
    if (!reference) {
        return {order, false};
    }
    const auto upper = static_cast<Price>(std::floor(*reference * (1.0 + band.width)));
    const auto lower = static_cast<Price>(std::ceil(*reference * (1.0 - band.width)));

    // A breach: the order would trade against a market already beyond the band.
    const NewOrder original = order;
    const bool market_order = order.type == OrderType::Market;
    bool breach = false;
    if (order.side == Side::Buy) {
        const auto ask = book_.best_ask();
        breach = ask && *ask > upper && (market_order || original.price >= *ask);
        if (market_order) {
            order.type = OrderType::ImmediateOrCancel;
            order.price = upper;
        } else {
            order.price = std::min(order.price, upper);
        }
    } else {
        const auto bid = book_.best_bid();
        breach = bid && *bid < lower && (market_order || original.price <= *bid);
        if (market_order) {
            order.type = OrderType::ImmediateOrCancel;
            order.price = lower;
        } else {
            order.price = std::max(order.price, lower);
        }
    }
    if (breach || (!market_order && order.price != original.price)) {
        ++band_adjusted_;
        ++agents_[agent].band_adjusted;
    }
    return {order, breach};
}

void Simulation::on_order_arrival(const Event& e) {
    if (now_ < halted_until_) {
        ++orders_rejected_in_halt_;
        return;
    }
    const BandResult banded = apply_price_band(e.order, e.agent);
    if (banded.breach && config_.price_band.halt_duration > 0) {
        // Limit up-limit down: pause trading instead of executing beyond the band.
        halted_until_ = now_ + config_.price_band.halt_duration;
        ++halts_;
        return;
    }
    const NewOrder& order = banded.order;
    const SubmitResult r = book_.submit(order);
    if (!r.accepted()) {
        return;
    }

    Qty taker_remaining = order.qty;
    for (std::size_t i = 0; i < r.trades.size(); ++i) {
        const Trade& t = r.trades[i];
        taker_remaining -= t.qty;
        const auto maker_order = book_.find(t.maker_id);
        const Qty maker_leaves = maker_order ? maker_order->remaining : 0;
        // After the last fill the taker's order is either resting or done.
        const Qty taker_leaves = i + 1 == r.trades.size() ? r.resting : taker_remaining;
        settle(t, maker_leaves, taker_leaves);
    }
    record_top_of_book();
}

void Simulation::on_cancel_arrival(const Event& e) {
    // Agents may only cancel their own orders.
    if (const auto o = book_.find(e.order.id); o && o->trader == e.agent) {
        book_.cancel(e.order.id);
        record_top_of_book();
    }
}

void Simulation::settle(const Trade& t, Qty maker_leaves, Qty taker_leaves) {
    const FeeSchedule& fees = config_.fees;
    const Money notional = t.price * t.qty * kMoneyPerTick;
    const Money taker_fee = fees.taker_fee * t.qty;
    const Money maker_fee = -fees.maker_rebate * t.qty;

    Account& maker = agents_[t.maker_trader].account;
    Account& taker = agents_[t.taker_trader].account;
    const bool taker_buys = t.taker_side == Side::Buy;

    taker.cash += taker_buys ? -notional : notional;
    maker.cash += taker_buys ? notional : -notional;
    taker.position += taker_buys ? t.qty : -t.qty;
    maker.position += taker_buys ? -t.qty : t.qty;

    taker.cash -= taker_fee;
    maker.cash -= maker_fee;
    taker.fees += taker_fee;
    maker.fees += maker_fee;
    exchange_revenue_ += taker_fee + maker_fee;

    taker.volume += t.qty;
    maker.volume += t.qty;
    ++taker.fills;
    ++maker.fills;

    last_trade_ = t.price;
    trades_.push_back(TradeRecord{now_, t.price, t.qty, t.taker_side, t.maker_trader, t.taker_trader});

    const Side maker_side = opposite(t.taker_side);
    Event to_maker{.time = now_ + agents_[t.maker_trader].latency,
                   .kind = EventKind::FillDelivery,
                   .agent = t.maker_trader};
    to_maker.fill = Fill{t.maker_id, maker_side, t.price, t.qty, maker_leaves, true, maker_fee, now_};
    schedule(std::move(to_maker));

    Event to_taker{.time = now_ + agents_[t.taker_trader].latency,
                   .kind = EventKind::FillDelivery,
                   .agent = t.taker_trader};
    to_taker.fill = Fill{t.taker_id, t.taker_side, t.price, t.qty, taker_leaves, false, taker_fee, now_};
    schedule(std::move(to_taker));
}

MarketView Simulation::current_view() const {
    MarketView v;
    v.as_of = now_;
    v.best_bid = book_.best_bid();
    v.best_ask = book_.best_ask();
    v.bid_size = v.best_bid ? book_.depth_at(Side::Buy, *v.best_bid) : 0;
    v.ask_size = v.best_ask ? book_.depth_at(Side::Sell, *v.best_ask) : 0;
    v.last_trade = last_trade_;
    return v;
}

void Simulation::record_top_of_book() {
    MarketView v = current_view();
    if (!v.same_market(tob_history_.back())) {
        tob_history_.push_back(v);
    }
}

MarketView Simulation::view_at(Time t) const {
    // Last recorded change at or before t.
    auto it = std::upper_bound(tob_history_.begin(), tob_history_.end(), t,
                               [](Time time, const MarketView& v) { return time < v.as_of; });
    MarketView v = it == tob_history_.begin() ? tob_history_.front() : *std::prev(it);
    v.as_of = t;
    return v;
}

// ---- AgentContext ------------------------------------------------------------

Time AgentContext::now() const noexcept {
    return sim_.now_;
}

Time AgentContext::latency() const noexcept {
    return sim_.agents_[id_].latency;
}

MarketView AgentContext::market() const {
    return sim_.view_at(sim_.now_ - latency());
}

double AgentContext::fundamental() const {
    return sim_.fundamental_at(sim_.now_ - latency());
}

OrderId AgentContext::submit(Side side, OrderType type, Price price, Qty qty) {
    const OrderId id = sim_.next_order_id_++;
    Simulation::Event e{.time = sim_.now_ + latency(), .kind = Simulation::EventKind::OrderArrival, .agent = id_};
    e.order = NewOrder{id, id_, side, type, price, qty};
    sim_.schedule(std::move(e));
    return id;
}

void AgentContext::cancel(OrderId id) {
    Simulation::Event e{.time = sim_.now_ + latency(), .kind = Simulation::EventKind::CancelArrival, .agent = id_};
    e.order.id = id;
    sim_.schedule(std::move(e));
}

void AgentContext::wake_after(Time delay) {
    sim_.schedule(Simulation::Event{.time = sim_.now_ + delay, .kind = Simulation::EventKind::Wakeup, .agent = id_});
}

} // namespace beaglex::sim
