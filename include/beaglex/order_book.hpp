#pragma once

#include "beaglex/types.hpp"

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace beaglex {

struct RestingOrder {
    OrderId id = 0;
    TraderId trader = 0;
    Side side = Side::Buy;
    Price price = 0;
    Qty remaining = 0;
};

struct SubmitResult {
    RejectReason reject = RejectReason::None;
    Qty filled = 0;    // executed immediately
    Qty resting = 0;   // left in the book
    Qty cancelled = 0; // unfilled remainder discarded (Market / IOC)
    std::vector<Trade> trades;

    bool accepted() const noexcept { return reject == RejectReason::None; }
};

struct LevelView {
    Price price = 0;
    Qty qty = 0;
    std::size_t orders = 0;

    bool operator==(const LevelView&) const = default;
};

// Limit order book for a single instrument with price-time priority.
//
// This is the straightforward baseline implementation (std::map of price levels,
// std::list FIFO per level, hash index for O(1) cancel lookup). It is the
// reference point that later optimisation work is benchmarked against.
//
// Time priority comes from arrival order, not wall-clock time, so the book is
// fully deterministic: the same input sequence always produces the same trades.
class OrderBook {
public:
    SubmitResult submit(const NewOrder& order);

    // Returns false if the order is not resting in the book.
    bool cancel(OrderId id);

    // Changes a resting order. Reducing quantity at the same price keeps time
    // priority; any other change is a cancel/replace that goes to the back of
    // the queue and may trade immediately if the new price crosses.
    SubmitResult modify(OrderId id, Price new_price, Qty new_qty);

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;

    Qty depth_at(Side side, Price price) const;
    std::vector<LevelView> levels(Side side, std::size_t max_levels = SIZE_MAX) const;

    const RestingOrder* find(OrderId id) const;
    std::size_t order_count() const noexcept { return index_.size(); }

private:
    using Queue = std::list<RestingOrder>;

    struct Level {
        Qty total = 0;
        Queue queue;
    };

    using Bids = std::map<Price, Level, std::greater<>>; // best (highest) first
    using Asks = std::map<Price, Level, std::less<>>;    // best (lowest) first

    struct Locator {
        Side side;
        Price price;
        Queue::iterator it;
    };

    template <class F>
    decltype(auto) with_book(Side side, F&& f) {
        return side == Side::Buy ? f(bids_) : f(asks_);
    }
    template <class F>
    decltype(auto) with_book(Side side, F&& f) const {
        return side == Side::Buy ? f(bids_) : f(asks_);
    }

    template <class Book>
    Qty match(Book& opposite, const NewOrder& taker, std::vector<Trade>& trades);

    void rest(const NewOrder& order, Qty qty);
    void unlink(const Locator& loc);

    Bids bids_;
    Asks asks_;
    std::unordered_map<OrderId, Locator> index_;
    SeqNum next_trade_seq_ = 1;
};

} // namespace beaglex
