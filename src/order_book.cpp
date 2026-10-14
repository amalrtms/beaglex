#include "beaglex/order_book.hpp"

#include <algorithm>

namespace beaglex {

namespace {

bool crosses(Side taker_side, Price limit, Price resting) noexcept {
    return taker_side == Side::Buy ? limit >= resting : limit <= resting;
}

// True if `a` is a strictly better price than `b` for resting orders on `side`.
bool better(Side side, Price a, Price b) noexcept {
    return side == Side::Buy ? a > b : a < b;
}

} // namespace

void OrderBook::reserve(std::size_t orders) {
    nodes_.reserve(orders);
    free_.reserve(orders);
    index_.reserve(orders);
    bids_.reserve(1024);
    asks_.reserve(1024);
}

std::size_t OrderBook::locate(const Levels& levels, Side side, Price price) noexcept {
    // Linear scan from the best end. Activity concentrates near the touch, so
    // this usually stops within a few levels, and a deeper insert has to shift
    // that many elements anyway.
    std::size_t pos = levels.size();
    while (pos > 0 && better(side, levels[pos - 1].price, price)) {
        --pos;
    }
    return pos;
}

OrderBook::Level* OrderBook::find_level(Side side, Price price) {
    Levels& levels = side_levels(side);
    const std::size_t pos = locate(levels, side, price);
    return pos > 0 && levels[pos - 1].price == price ? &levels[pos - 1] : nullptr;
}

const OrderBook::Level* OrderBook::find_level(Side side, Price price) const {
    const Levels& levels = side_levels(side);
    const std::size_t pos = locate(levels, side, price);
    return pos > 0 && levels[pos - 1].price == price ? &levels[pos - 1] : nullptr;
}

SubmitResult OrderBook::submit(const NewOrder& order) {
    SubmitResult result;

    if (order.qty <= 0) {
        result.reject = RejectReason::InvalidQuantity;
        return result;
    }
    if (order.type != OrderType::Market && order.price <= 0) {
        result.reject = RejectReason::InvalidPrice;
        return result;
    }
    if (index_.contains(order.id)) {
        result.reject = RejectReason::DuplicateOrderId;
        return result;
    }

    trades_.clear(); // keeps capacity: no allocation once warmed up
    const Qty remaining = match(order);
    result.trades = trades_;
    result.filled = order.qty - remaining;

    if (remaining > 0) {
        if (order.type == OrderType::Limit) {
            rest(order, remaining);
            result.resting = remaining;
        } else {
            result.cancelled = remaining;
        }
    }
    return result;
}

Qty OrderBook::match(const NewOrder& taker) {
    Levels& book = side_levels(opposite(taker.side));
    Qty remaining = taker.qty;
    const bool any_price = taker.type == OrderType::Market;

    while (remaining > 0 && !book.empty()) {
        Level& level = book.back();
        if (!any_price && !crosses(taker.side, taker.price, level.price)) {
            break;
        }

        while (remaining > 0 && level.head != kNull) {
            const NodeIdx n = level.head;
            Node& maker = nodes_[n];
            const Qty qty = std::min(remaining, maker.remaining);

            trades_.push_back(Trade{
                .seq = next_trade_seq_++,
                .maker_id = maker.id,
                .taker_id = taker.id,
                .maker_trader = maker.trader,
                .taker_trader = taker.trader,
                .taker_side = taker.side,
                .price = level.price,
                .qty = qty,
            });

            remaining -= qty;
            maker.remaining -= qty;
            level.total -= qty;
            if (maker.remaining == 0) {
                index_.erase(maker.id);
                unlink(level, n);
            }
        }

        if (level.head == kNull) {
            book.pop_back();
        }
    }
    return remaining;
}

OrderBook::NodeIdx OrderBook::alloc_node() {
    if (!free_.empty()) {
        const NodeIdx n = free_.back();
        free_.pop_back();
        return n;
    }
    nodes_.emplace_back();
    return static_cast<NodeIdx>(nodes_.size() - 1);
}

// Removes node `n` from its level's queue and returns its slot to the pool.
// Does not adjust level.total; callers account for the quantity themselves.
void OrderBook::unlink(Level& level, NodeIdx n) {
    const Node& node = nodes_[n];
    if (node.prev != kNull) {
        nodes_[node.prev].next = node.next;
    } else {
        level.head = node.next;
    }
    if (node.next != kNull) {
        nodes_[node.next].prev = node.prev;
    } else {
        level.tail = node.prev;
    }
    --level.count;
    free_.push_back(n);
}

void OrderBook::rest(const NewOrder& order, Qty qty) {
    const NodeIdx n = alloc_node(); // may grow nodes_, so take no references before this

    Levels& levels = side_levels(order.side);
    const std::size_t pos = locate(levels, order.side, order.price);
    Level* level = nullptr;
    if (pos > 0 && levels[pos - 1].price == order.price) {
        level = &levels[pos - 1];
    } else {
        level = &*levels.insert(levels.begin() + static_cast<std::ptrdiff_t>(pos), Level{.price = order.price});
    }

    nodes_[n] = Node{order.id, order.price, qty, order.trader, order.side, level->tail, kNull};
    if (level->tail != kNull) {
        nodes_[level->tail].next = n;
    } else {
        level->head = n;
    }
    level->tail = n;
    ++level->count;
    level->total += qty;
    index_.insert(order.id, n);
}

bool OrderBook::cancel(OrderId id) {
    const NodeIdx n = index_.find(id);
    if (n == detail::OrderIndex::kNotFound) {
        return false;
    }
    const Node& node = nodes_[n];
    Levels& levels = side_levels(node.side);
    const std::size_t pos = locate(levels, node.side, node.price) - 1; // level must exist
    Level& level = levels[pos];

    level.total -= node.remaining;
    unlink(level, n);
    if (level.head == kNull) {
        levels.erase(levels.begin() + static_cast<std::ptrdiff_t>(pos));
    }
    index_.erase(id);
    return true;
}

SubmitResult OrderBook::modify(OrderId id, Price new_price, Qty new_qty) {
    SubmitResult result;

    const NodeIdx n = index_.find(id);
    if (n == detail::OrderIndex::kNotFound) {
        result.reject = RejectReason::UnknownOrder;
        return result;
    }
    if (new_qty <= 0) {
        result.reject = RejectReason::InvalidQuantity;
        return result;
    }
    if (new_price <= 0) {
        result.reject = RejectReason::InvalidPrice;
        return result;
    }

    Node& node = nodes_[n];

    // Pure size reduction: keep the order's place in the queue.
    if (new_price == node.price && new_qty <= node.remaining) {
        find_level(node.side, node.price)->total -= node.remaining - new_qty;
        node.remaining = new_qty;
        result.resting = new_qty;
        return result;
    }

    // Otherwise cancel/replace: loses priority and may cross the spread.
    const NewOrder replacement{id, node.trader, node.side, OrderType::Limit, new_price, new_qty};
    cancel(id);
    return submit(replacement);
}

std::optional<Price> OrderBook::best_bid() const {
    if (bids_.empty()) {
        return std::nullopt;
    }
    return bids_.back().price;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) {
        return std::nullopt;
    }
    return asks_.back().price;
}

Qty OrderBook::depth_at(Side side, Price price) const {
    const Level* level = find_level(side, price);
    return level ? level->total : 0;
}

std::vector<LevelView> OrderBook::levels(Side side, std::size_t max_levels) const {
    const Levels& levels = side_levels(side);
    std::vector<LevelView> out;
    for (auto it = levels.rbegin(); it != levels.rend() && out.size() < max_levels; ++it) {
        out.push_back(LevelView{it->price, it->total, it->count});
    }
    return out;
}

std::optional<RestingOrder> OrderBook::find(OrderId id) const {
    const NodeIdx idx = index_.find(id);
    if (idx == detail::OrderIndex::kNotFound) {
        return std::nullopt;
    }
    const Node& n = nodes_[idx];
    return RestingOrder{n.id, n.trader, n.side, n.price, n.remaining};
}

} // namespace beaglex
