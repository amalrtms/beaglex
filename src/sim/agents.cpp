#include "beaglex/sim/agents.hpp"

#include <algorithm>
#include <cmath>

namespace beaglex::sim {

namespace {

Price to_price(double ticks) {
    return std::max<Price>(1, static_cast<Price>(std::llround(ticks)));
}

// Best available guess at "the price": mid, else last trade, else one side.
std::optional<double> reference_price(const MarketView& v) {
    if (auto m = v.mid()) {
        return m;
    }
    if (v.last_trade) {
        return static_cast<double>(*v.last_trade);
    }
    if (v.best_bid) {
        return static_cast<double>(*v.best_bid);
    }
    if (v.best_ask) {
        return static_cast<double>(*v.best_ask);
    }
    return std::nullopt;
}

} // namespace

// ---- NoiseTrader ---------------------------------------------------------------

void NoiseTrader::on_start(AgentContext& ctx) {
    ctx.wake_after(exponential(cfg_.mean_interval));
}

Time NoiseTrader::next_interval() {
    if (cfg_.activity_sensitivity == 0.0) {
        return exponential(cfg_.mean_interval);
    }
    const double excitement = vol_.sigma() / cfg_.reference_volatility - 1.0;
    const double intensity = std::clamp(1.0 + cfg_.activity_sensitivity * excitement, 0.5, cfg_.max_intensity);
    return exponential(static_cast<Time>(static_cast<double>(cfg_.mean_interval) / intensity));
}

void NoiseTrader::on_wakeup(AgentContext& ctx) {
    const MarketView view = ctx.market();
    const double ref = reference_price(view).value_or(static_cast<double>(cfg_.fallback_price));
    vol_.update(ctx.now(), ref);
    ctx.wake_after(next_interval());

    // At most one working order: replace rather than accumulate.
    if (resting_) {
        ctx.cancel(*resting_);
        resting_.reset();
    }

    Side side = uniform() < 0.5 ? Side::Buy : Side::Sell;
    if (position_ >= cfg_.max_position) {
        side = Side::Sell;
    } else if (position_ <= -cfg_.max_position) {
        side = Side::Buy;
    }
    const Qty qty = cfg_.lot * uniform_int(1, cfg_.max_lots);

    if (uniform() < cfg_.market_order_prob) {
        ctx.submit(side, OrderType::Market, 0, qty);
        return;
    }
    // Positive offset rests behind the mid, negative crosses it.
    const auto offset = static_cast<double>(uniform_int(-cfg_.max_cross, cfg_.max_offset));
    const double price = side == Side::Buy ? ref - offset : ref + offset;
    resting_ = ctx.submit(side, OrderType::Limit, to_price(price), qty);
}

void NoiseTrader::on_fill(AgentContext& ctx, const Fill& fill) {
    Agent::on_fill(ctx, fill);
    if (resting_ && fill.order_id == *resting_ && fill.leaves == 0) {
        resting_.reset();
    }
}

// ---- MarketMaker ---------------------------------------------------------------

void MarketMaker::on_start(AgentContext& ctx) {
    ctx.wake_after(uniform_int(1, cfg_.interval));
}

void MarketMaker::on_wakeup(AgentContext& ctx) {
    ctx.wake_after(cfg_.interval + uniform_int(0, cfg_.interval / 10));

    const double ref = reference_price(ctx.market()).value_or(static_cast<double>(cfg_.fallback_price));
    if (ctx.now() - last_vol_sample_ >= kSecond) {
        fast_vol_.update(ctx.now(), ref);
        slow_vol_.update(ctx.now(), ref);
        last_vol_sample_ = ctx.now();
    }
    // Long inventory pushes quotes down (sell more readily), short pushes them up.
    const double centre = ref - cfg_.skew_per_share * static_cast<double>(position_);
    // Unusually volatile markets carry more adverse-selection risk: quote wider.
    const double stress = std::max(0.0, fast_vol_.sigma() / slow_vol_.sigma() - 1.0);
    const double half_spread = cfg_.half_spread + cfg_.stress_spread * stress;
    const auto bid = static_cast<Price>(std::floor(centre - half_spread));
    auto ask = static_cast<Price>(std::ceil(centre + half_spread));
    if (ask <= bid) {
        ask = bid + 1;
    }

    const bool want_bid = position_ + cfg_.quote_size <= cfg_.max_position;
    const bool want_ask = position_ - cfg_.quote_size >= -cfg_.max_position;
    requote(ctx, bid_, Side::Buy, std::max<Price>(1, bid), want_bid);
    requote(ctx, ask_, Side::Sell, std::max<Price>(1, ask), want_ask);
}

void MarketMaker::requote(AgentContext& ctx, std::optional<Quote>& quote, Side side, Price price, bool want) {
    // Only touch the order when something changed, keeping queue priority otherwise.
    if (quote && (!want || quote->price != price)) {
        ctx.cancel(quote->id);
        quote.reset();
    }
    if (want && !quote) {
        quote = Quote{ctx.submit(side, OrderType::Limit, price, cfg_.quote_size), price};
    }
}

void MarketMaker::on_fill(AgentContext& ctx, const Fill& fill) {
    Agent::on_fill(ctx, fill);
    for (auto* q : {&bid_, &ask_}) {
        if (*q && (*q)->id == fill.order_id && fill.leaves == 0) {
            q->reset();
        }
    }
}

// ---- MomentumTrader ------------------------------------------------------------

void MomentumTrader::on_start(AgentContext& ctx) {
    ctx.wake_after(exponential(cfg_.mean_interval));
}

void MomentumTrader::on_wakeup(AgentContext& ctx) {
    ctx.wake_after(exponential(cfg_.mean_interval));

    const MarketView view = ctx.market();
    const auto ref = reference_price(view);
    if (!ref) {
        return;
    }
    if (!initialised_) {
        fast_ = slow_ = *ref;
        last_update_ = ctx.now();
        initialised_ = true;
        return;
    }

    // Time-aware EMAs: wake-ups are irregular, so weight by elapsed time.
    const auto dt = static_cast<double>(ctx.now() - last_update_);
    last_update_ = ctx.now();
    fast_ += (1.0 - std::exp(-dt / static_cast<double>(cfg_.fast_tau))) * (*ref - fast_);
    slow_ += (1.0 - std::exp(-dt / static_cast<double>(cfg_.slow_tau))) * (*ref - slow_);
    if (ctx.now() < cfg_.warmup) {
        return;
    }

    const double signal = (fast_ - slow_) / slow_;
    const Qty size = cfg_.lot * uniform_int(1, cfg_.max_lots);

    if (signal > cfg_.threshold && view.best_ask) {
        const Qty qty = std::min(size, cfg_.max_position - position_);
        if (qty > 0) {
            ctx.submit(Side::Buy, OrderType::ImmediateOrCancel, *view.best_ask + cfg_.max_slippage, qty);
        }
    } else if (signal < -cfg_.threshold && view.best_bid) {
        const Qty qty = std::min(size, cfg_.max_position + position_);
        if (qty > 0) {
            ctx.submit(Side::Sell, OrderType::ImmediateOrCancel, std::max<Price>(1, *view.best_bid - cfg_.max_slippage), qty);
        }
    } else if (std::abs(signal) < cfg_.threshold / 2 && position_ != 0) {
        // Trend has faded: unwind a lot.
        const Qty qty = std::min(cfg_.lot, std::abs(position_));
        if (position_ > 0 && view.best_bid) {
            ctx.submit(Side::Sell, OrderType::ImmediateOrCancel, std::max<Price>(1, *view.best_bid - cfg_.max_slippage), qty);
        } else if (position_ < 0 && view.best_ask) {
            ctx.submit(Side::Buy, OrderType::ImmediateOrCancel, *view.best_ask + cfg_.max_slippage, qty);
        }
    }
}

// ---- ValueTrader ---------------------------------------------------------------

ValueTrader::ValueTrader(const ValueTraderConfig& cfg, std::uint64_t seed) : Agent(seed), cfg_(cfg) {
    bias_ = normal(0.0, cfg_.bias_sigma);
}

void ValueTrader::on_start(AgentContext& ctx) {
    ctx.wake_after(exponential(cfg_.mean_interval));
}

void ValueTrader::on_wakeup(AgentContext& ctx) {
    ctx.wake_after(exponential(cfg_.mean_interval));

    const MarketView view = ctx.market();
    const double value = ctx.fundamental() * (1.0 + bias_ + normal(0.0, cfg_.noise_sigma));
    const double buy_below = value * (1.0 - cfg_.threshold);
    const double sell_above = value * (1.0 + cfg_.threshold);

    // Size scales with how mispriced the market looks.
    auto lots_for = [&](double mispricing) {
        return std::clamp<std::int64_t>(static_cast<std::int64_t>(mispricing / cfg_.threshold), 1, cfg_.max_lots);
    };

    if (view.best_ask && static_cast<double>(*view.best_ask) < buy_below) {
        const double mispricing = 1.0 - static_cast<double>(*view.best_ask) / value;
        const Qty qty = std::min(cfg_.lot * lots_for(mispricing), cfg_.max_position - position_);
        if (qty > 0) {
            ctx.submit(Side::Buy, OrderType::ImmediateOrCancel, static_cast<Price>(std::floor(buy_below)), qty);
        }
    } else if (view.best_bid && static_cast<double>(*view.best_bid) > sell_above) {
        const double mispricing = static_cast<double>(*view.best_bid) / value - 1.0;
        const Qty qty = std::min(cfg_.lot * lots_for(mispricing), cfg_.max_position + position_);
        if (qty > 0) {
            ctx.submit(Side::Sell, OrderType::ImmediateOrCancel, static_cast<Price>(std::ceil(sell_above)), qty);
        }
    } else if (position_ > 0 && view.best_bid && static_cast<double>(*view.best_bid) >= value) {
        // Price has converged: take profit, freeing capacity for the next mispricing.
        ctx.submit(Side::Sell, OrderType::ImmediateOrCancel, *view.best_bid, std::min(cfg_.lot * cfg_.max_lots, position_));
    } else if (position_ < 0 && view.best_ask && static_cast<double>(*view.best_ask) <= value) {
        ctx.submit(Side::Buy, OrderType::ImmediateOrCancel, *view.best_ask, std::min(cfg_.lot * cfg_.max_lots, -position_));
    }
}

} // namespace beaglex::sim
