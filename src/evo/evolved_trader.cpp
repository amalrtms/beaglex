#include "beaglex/evo/evolved_trader.hpp"

#include <algorithm>
#include <cmath>

namespace beaglex::evo {

namespace {

double clamp3(double x) {
    return std::clamp(x, -3.0, 3.0);
}

} // namespace

double EvolvedTrader::score(const Signals& s) const noexcept {
    return genome_[kMomentum] * s.momentum + genome_[kReversion] * s.reversion + genome_[kImbalance] * s.imbalance +
           genome_[kInventory] * s.inventory;
}

sim::Time EvolvedTrader::next_interval() {
    const double seconds = std::pow(10.0, genome_[kLogInterval]);
    return exponential(static_cast<sim::Time>(seconds * static_cast<double>(sim::kSecond)));
}

void EvolvedTrader::on_start(sim::AgentContext& ctx) {
    ctx.wake_after(next_interval());
}

void EvolvedTrader::on_wakeup(sim::AgentContext& ctx) {
    ctx.wake_after(next_interval());

    const sim::MarketView view = ctx.market();
    double ref = static_cast<double>(cfg_.fallback_price);
    if (const auto mid = view.mid()) {
        ref = *mid;
    } else if (view.last_trade) {
        ref = static_cast<double>(*view.last_trade);
    }

    if (!initialised_) {
        fast_ = slow_ = ref;
        last_update_ = ctx.now();
        initialised_ = true;
    }
    const auto dt = static_cast<double>(ctx.now() - last_update_);
    last_update_ = ctx.now();
    fast_ += (1.0 - std::exp(-dt / static_cast<double>(cfg_.fast_tau))) * (ref - fast_);
    slow_ += (1.0 - std::exp(-dt / static_cast<double>(cfg_.slow_tau))) * (ref - slow_);

    Signals s;
    s.momentum = clamp3((fast_ - slow_) / slow_ / 1e-3);
    s.reversion = clamp3((ref - fast_) / ref / 1e-3);
    const auto depth = static_cast<double>(view.bid_size + view.ask_size);
    s.imbalance = depth > 0 ? static_cast<double>(view.bid_size - view.ask_size) / depth : 0.0;
    s.inventory = static_cast<double>(position_) / static_cast<double>(cfg_.max_position);
    const double sc = score(s);

    if (genome_.quotes()) {
        quote(ctx, view, ref, sc);
    } else {
        trade_directionally(ctx, view, sc);
    }
}

void EvolvedTrader::trade_directionally(sim::AgentContext& ctx, const sim::MarketView& view, double sc) {
    // One working passive order at a time: replace it on every decision.
    if (working_) {
        ctx.cancel(*working_);
        working_.reset();
    }
    if (std::abs(sc) < genome_[kThreshold]) {
        return;
    }

    const Side side = sc > 0 ? Side::Buy : Side::Sell;
    const Qty room = side == Side::Buy ? cfg_.max_position - position_ : cfg_.max_position + position_;
    const Qty qty = std::min(cfg_.lot * static_cast<Qty>(std::llround(genome_[kLots])), room);
    if (qty <= 0) {
        return;
    }

    if (uniform() < genome_[kAggression]) {
        // Take: IOC through the opposite touch.
        if (side == Side::Buy && view.best_ask) {
            ctx.submit(side, OrderType::ImmediateOrCancel, *view.best_ask + cfg_.slippage, qty);
        } else if (side == Side::Sell && view.best_bid) {
            ctx.submit(side, OrderType::ImmediateOrCancel, std::max<Price>(1, *view.best_bid - cfg_.slippage), qty);
        }
    } else {
        // Make: join the best price on our own side.
        const auto touch = side == Side::Buy ? view.best_bid : view.best_ask;
        if (touch) {
            working_ = ctx.submit(side, OrderType::Limit, *touch, qty);
        }
    }
}

void EvolvedTrader::quote(sim::AgentContext& ctx, const sim::MarketView& view, double ref, double sc) {
    const double centre = ref + sc; // the score leans the quotes, in ticks
    const double hs = genome_[kHalfSpread];
    auto bid = std::max<Price>(1, static_cast<Price>(std::floor(centre - hs)));
    auto ask = static_cast<Price>(std::ceil(centre + hs));
    // Post-only, as market makers use on real venues: quotes add liquidity
    // and never cross the market, however far the score leans them.
    if (view.best_ask) {
        bid = std::min(bid, *view.best_ask - 1);
    }
    if (view.best_bid) {
        ask = std::max(ask, *view.best_bid + 1);
    }
    if (ask <= bid) {
        ask = bid + 1;
    }
    const Qty size = cfg_.lot * static_cast<Qty>(std::llround(genome_[kLots]));
    requote(ctx, bid_, Side::Buy, bid, position_ + size <= cfg_.max_position);
    requote(ctx, ask_, Side::Sell, ask, position_ - size >= -cfg_.max_position);
}

void EvolvedTrader::requote(sim::AgentContext& ctx, std::optional<Quote>& q, Side side, Price price, bool want) {
    if (q && (!want || q->price != price)) {
        ctx.cancel(q->id);
        q.reset();
    }
    if (want && !q) {
        const Qty size = cfg_.lot * static_cast<Qty>(std::llround(genome_[kLots]));
        q = Quote{ctx.submit(side, OrderType::Limit, price, size), price};
    }
}

void EvolvedTrader::on_fill(sim::AgentContext& ctx, const sim::Fill& fill) {
    Agent::on_fill(ctx, fill);
    if (fill.leaves != 0) {
        return;
    }
    if (working_ && *working_ == fill.order_id) {
        working_.reset();
    }
    for (auto* q : {&bid_, &ask_}) {
        if (*q && (*q)->id == fill.order_id) {
            q->reset();
        }
    }
}

} // namespace beaglex::evo
