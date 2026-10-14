#pragma once

// A deliberately naive order book used as a test oracle. It keeps every
// resting order in one flat vector and linearly scans for the best match on
// every step. Slow, but simple enough to be obviously correct, so the real
// order books can be fuzzed against it.

#include "beaglex/types.hpp"

#include <algorithm>
#include <map>
#include <ostream>
#include <vector>

namespace beaglex {

// Readable GoogleTest output for failed comparisons (found via ADL).
inline void PrintTo(const Trade& t, std::ostream* os) {
    *os << "Trade{seq=" << t.seq << " maker=#" << t.maker_id << " taker=#" << t.taker_id << ' '
        << to_string(t.taker_side) << ' ' << t.qty << " @ " << format_price(t.price) << '}';
}

inline void PrintTo(const LevelView& l, std::ostream* os) {
    *os << '{' << format_price(l.price) << " x " << l.qty << " (" << l.orders << " orders)}";
}

} // namespace beaglex

namespace beaglex::test {

class ReferenceBook {
public:
    OwningSubmitResult submit(const NewOrder& o) {
        OwningSubmitResult r;
        if (o.qty <= 0) {
            r.reject = RejectReason::InvalidQuantity;
            return r;
        }
        if (o.type != OrderType::Market && o.price <= 0) {
            r.reject = RejectReason::InvalidPrice;
            return r;
        }
        if (find_index(o.id) >= 0) {
            r.reject = RejectReason::DuplicateOrderId;
            return r;
        }

        Qty remaining = o.qty;
        while (remaining > 0) {
            const int best = best_opposite(o.side);
            if (best < 0) {
                break;
            }
            Entry& maker = orders_[static_cast<std::size_t>(best)];
            const bool crosses = o.side == Side::Buy ? o.price >= maker.price : o.price <= maker.price;
            if (o.type != OrderType::Market && !crosses) {
                break;
            }
            const Qty qty = std::min(remaining, maker.qty);
            r.trades.push_back(Trade{seq_++, maker.id, o.id, maker.trader, o.trader, o.side, maker.price, qty});
            remaining -= qty;
            maker.qty -= qty;
            if (maker.qty == 0) {
                orders_.erase(orders_.begin() + best);
            }
        }

        r.filled = o.qty - remaining;
        if (remaining > 0) {
            if (o.type == OrderType::Limit) {
                orders_.push_back(Entry{o.id, o.trader, o.side, o.price, remaining, arrival_++});
                r.resting = remaining;
            } else {
                r.cancelled = remaining;
            }
        }
        return r;
    }

    bool cancel(OrderId id) {
        const int i = find_index(id);
        if (i < 0) {
            return false;
        }
        orders_.erase(orders_.begin() + i);
        return true;
    }

    OwningSubmitResult modify(OrderId id, Price new_price, Qty new_qty) {
        OwningSubmitResult r;
        const int i = find_index(id);
        if (i < 0) {
            r.reject = RejectReason::UnknownOrder;
            return r;
        }
        if (new_qty <= 0) {
            r.reject = RejectReason::InvalidQuantity;
            return r;
        }
        if (new_price <= 0) {
            r.reject = RejectReason::InvalidPrice;
            return r;
        }
        Entry& e = orders_[static_cast<std::size_t>(i)];
        if (new_price == e.price && new_qty <= e.qty) {
            e.qty = new_qty;
            r.resting = new_qty;
            return r;
        }
        const NewOrder replacement{id, e.trader, e.side, OrderType::Limit, new_price, new_qty};
        cancel(id);
        return submit(replacement);
    }

    std::vector<LevelView> levels(Side side) const {
        std::map<Price, LevelView> by_price;
        for (const auto& e : orders_) {
            if (e.side != side) {
                continue;
            }
            LevelView& lv = by_price[e.price];
            lv.price = e.price;
            lv.qty += e.qty;
            ++lv.orders;
        }
        std::vector<LevelView> out;
        for (const auto& [price, lv] : by_price) {
            out.push_back(lv);
        }
        if (side == Side::Buy) {
            std::reverse(out.begin(), out.end());
        }
        return out;
    }

    std::size_t order_count() const { return orders_.size(); }

private:
    struct Entry {
        OrderId id;
        TraderId trader;
        Side side;
        Price price;
        Qty qty;
        std::uint64_t arrival;
    };

    int find_index(OrderId id) const {
        for (std::size_t i = 0; i < orders_.size(); ++i) {
            if (orders_[i].id == id) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    // Best resting order on the side opposite to `taker`: best price, then earliest arrival.
    int best_opposite(Side taker) const {
        int best = -1;
        for (std::size_t i = 0; i < orders_.size(); ++i) {
            const Entry& e = orders_[i];
            if (e.side == taker) {
                continue;
            }
            if (best < 0) {
                best = static_cast<int>(i);
                continue;
            }
            const Entry& b = orders_[static_cast<std::size_t>(best)];
            const bool better_price = e.side == Side::Sell ? e.price < b.price : e.price > b.price;
            if (better_price || (e.price == b.price && e.arrival < b.arrival)) {
                best = static_cast<int>(i);
            }
        }
        return best;
    }

    std::vector<Entry> orders_;
    std::uint64_t arrival_ = 0;
    SeqNum seq_ = 1;
};

} // namespace beaglex::test
