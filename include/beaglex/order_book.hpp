#pragma once

#include "beaglex/detail/order_index.hpp"
#include "beaglex/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace beaglex {

// Limit order book for a single instrument with price-time priority.
//
// Time priority comes from arrival order, not wall-clock time, so the book is
// fully deterministic: the same input sequence always produces the same trades.
//
// This is the optimised implementation; BaselineOrderBook is the original it is
// benchmarked against. The hot path performs no heap allocation once warmed up:
//  - price levels are a contiguous vector per side, best price at the back;
//  - orders live in a recycled pool, queued per level by index;
//  - order ids map to pool slots through a flat open-addressing hash index;
//  - trades are written to a reused buffer.
class OrderBook {
public:
    // Pre-allocates storage so the first `orders` resting orders never allocate.
    void reserve(std::size_t orders);

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

    std::optional<RestingOrder> find(OrderId id) const;
    std::size_t order_count() const noexcept { return index_.size(); }

private:
    using NodeIdx = std::uint32_t;
    static constexpr NodeIdx kNull = UINT32_MAX;

    struct Node {
        OrderId id;
        Price price;
        Qty remaining;
        TraderId trader;
        Side side;
        NodeIdx prev;
        NodeIdx next;
    };

    struct Level {
        Price price;
        Qty total = 0;
        NodeIdx head = kNull;
        NodeIdx tail = kNull;
        std::uint32_t count = 0;
    };

    // Sorted worst-to-best, so the best price is levels.back(): matching only
    // ever touches the end of the vector, and since most orders arrive near
    // the touch, inserting or erasing a level shifts only a few elements.
    using Levels = std::vector<Level>;

    Levels& side_levels(Side side) noexcept { return side == Side::Buy ? bids_ : asks_; }
    const Levels& side_levels(Side side) const noexcept { return side == Side::Buy ? bids_ : asks_; }

    // Position of `price` in `levels`: the index one past the last level that
    // is not strictly better. levels[pos - 1] holds `price` if it exists.
    static std::size_t locate(const Levels& levels, Side side, Price price) noexcept;
    Level* find_level(Side side, Price price);
    const Level* find_level(Side side, Price price) const;

    Qty match(const NewOrder& taker);
    void rest(const NewOrder& order, Qty qty);
    NodeIdx alloc_node();
    void unlink(Level& level, NodeIdx n);

    Levels bids_; // ascending price
    Levels asks_; // descending price
    std::vector<Node> nodes_;
    std::vector<NodeIdx> free_;
    detail::OrderIndex index_;
    std::vector<Trade> trades_; // reused output buffer behind SubmitResult::trades
    SeqNum next_trade_seq_ = 1;
};

} // namespace beaglex
