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

// The first, straightforward order book: std::map of price levels, std::list
// FIFO per level, std::unordered_map index for O(1) cancel lookup, and a fresh
// std::vector of trades per call.
//
// Kept unchanged as the "before" in benchmarks and as a second implementation
// for the differential fuzz test. See OrderBook for the optimised version.
class BaselineOrderBook {
public:
    OwningSubmitResult submit(const NewOrder& order);
    bool cancel(OrderId id);
    OwningSubmitResult modify(OrderId id, Price new_price, Qty new_qty);

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;

    Qty depth_at(Side side, Price price) const;
    std::vector<LevelView> levels(Side side, std::size_t max_levels = SIZE_MAX) const;

    std::optional<RestingOrder> find(OrderId id) const;
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
