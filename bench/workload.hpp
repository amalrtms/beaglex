#pragma once

// Deterministic synthetic order flow for benchmarking.
//
// The mix is loosely modelled on real equity order flow: most activity is
// passive limit orders near the touch and cancellations (real venues see far
// more cancels than trades), with a minority of aggressive orders that trade.
// A reference book is run during generation so cancels and modifies target
// orders that are actually live, as they would in a real feed.

#include "beaglex/baseline_order_book.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <random>
#include <vector>

namespace beaglex::bench {

struct Op {
    enum class Kind : std::uint8_t { Submit, Cancel, Modify };
    Kind kind = Kind::Submit;
    NewOrder order; // Cancel/Modify use order.id, and Modify uses price/qty as the new values
};

struct WorkloadConfig {
    std::uint64_t seed = 42;
    std::size_t prefill = 10'000;   // passive orders placed before measuring
    std::size_t ops = 2'000'000;    // measured operations
    std::size_t target_depth = 10'000; // cancels pick up when the book is larger than this
};

struct Workload {
    std::vector<Op> prefill;
    std::vector<Op> ops;
};

struct WorkloadStats {
    std::size_t submits = 0, cancels = 0, modifies = 0, trades = 0;
    std::size_t final_orders = 0;
    double avg_orders = 0; // mean resting orders over the measured run
};

inline Workload generate_workload(const WorkloadConfig& cfg, WorkloadStats* stats = nullptr) {
    std::mt19937_64 rng(cfg.seed);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::exponential_distribution<double> depth_offset(1.0 / 8.0); // mean 8 ticks from the touch
    auto uniform = [&](std::int64_t lo, std::int64_t hi) {
        return std::uniform_int_distribution<std::int64_t>(lo, hi)(rng);
    };

    BaselineOrderBook book;
    std::vector<OrderId> live; // superset of live ids; stale entries are dropped lazily
    OrderId next_id = 1;
    Price fair = 10'000;
    WorkloadStats local;
    double depth_sum = 0;

    auto random_trader = [&] { return static_cast<TraderId>(uniform(1, 64)); };
    auto lot = [&] { return uniform(1, 10) * 10; };

    auto passive_order = [&] {
        NewOrder o;
        o.id = next_id++;
        o.trader = random_trader();
        o.side = unit(rng) < 0.5 ? Side::Buy : Side::Sell;
        o.type = OrderType::Limit;
        const auto offset = 1 + static_cast<Price>(std::min(depth_offset(rng), 200.0));
        o.price = o.side == Side::Buy ? fair - offset : fair + offset;
        o.qty = lot();
        return o;
    };

    // Picks a random live order, discarding ids that have since filled or been cancelled.
    auto pick_live = [&]() -> std::optional<RestingOrder> {
        while (!live.empty()) {
            const auto i = static_cast<std::size_t>(uniform(0, static_cast<std::int64_t>(live.size()) - 1));
            if (auto o = book.find(live[i])) {
                return o;
            }
            live[i] = live.back();
            live.pop_back();
        }
        return std::nullopt;
    };

    auto record_submit = [&](std::vector<Op>& out, const NewOrder& o) {
        const auto r = book.submit(o);
        local.trades += r.trades.size();
        ++local.submits;
        if (r.resting > 0) {
            live.push_back(o.id);
        }
        out.push_back({Op::Kind::Submit, o});
    };

    Workload w;
    w.prefill.reserve(cfg.prefill);
    w.ops.reserve(cfg.ops);
    for (std::size_t i = 0; i < cfg.prefill; ++i) {
        record_submit(w.prefill, passive_order());
    }

    while (w.ops.size() < cfg.ops) {
        // Fair value drifts as a random walk; aggressive orders chase it.
        if (unit(rng) < 0.05) {
            fair += unit(rng) < 0.5 ? -1 : 1;
        }

        const double cancel_p = book.order_count() > cfg.target_depth ? 0.40 : 0.22;
        depth_sum += static_cast<double>(book.order_count());
        const double r = unit(rng);

        if (r < cancel_p) {
            if (auto o = pick_live()) {
                book.cancel(o->id);
                ++local.cancels;
                w.ops.push_back({Op::Kind::Cancel, NewOrder{.id = o->id}});
            }
        } else if (r < cancel_p + 0.07) {
            if (auto o = pick_live()) {
                NewOrder m{.id = o->id};
                if (unit(rng) < 0.7 && o->remaining > 1) {
                    m.price = o->price; // shrink: keeps queue priority
                    m.qty = std::max<Qty>(1, o->remaining * uniform(3, 9) / 10);
                } else {
                    m.price = o->price + (o->side == Side::Buy ? uniform(-3, 2) : uniform(-2, 3));
                    m.qty = o->remaining;
                }
                local.trades += book.modify(m.id, m.price, m.qty).trades.size();
                ++local.modifies;
                w.ops.push_back({Op::Kind::Modify, m});
            }
        } else if (r < cancel_p + 0.07 + 0.08) {
            // Aggressive: marketable limit, IOC or market order.
            NewOrder o;
            o.id = next_id++;
            o.trader = random_trader();
            o.side = unit(rng) < 0.5 ? Side::Buy : Side::Sell;
            const double t = unit(rng);
            o.type = t < 0.6 ? OrderType::Limit : t < 0.9 ? OrderType::ImmediateOrCancel : OrderType::Market;
            const Price reach = uniform(0, 4);
            o.price = o.side == Side::Buy ? fair + reach : fair - reach;
            o.qty = lot() * uniform(1, 3);
            record_submit(w.ops, o);
        } else {
            record_submit(w.ops, passive_order());
        }
    }

    local.final_orders = book.order_count();
    local.avg_orders = depth_sum / static_cast<double>(w.ops.size());
    if (stats) {
        *stats = local;
    }
    return w;
}

} // namespace beaglex::bench
