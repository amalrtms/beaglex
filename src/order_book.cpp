#include "beaglex/order_book.hpp"

#include <algorithm>
#include <iterator>

namespace beaglex {

namespace {

bool crosses(Side taker_side, Price limit, Price resting) noexcept {
    return taker_side == Side::Buy ? limit >= resting : limit <= resting;
}

} // namespace

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

    const Qty remaining = order.side == Side::Buy ? match(asks_, order, result.trades)
                                                  : match(bids_, order, result.trades);
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

template <class Book>
Qty OrderBook::match(Book& opposite, const NewOrder& taker, std::vector<Trade>& trades) {
    Qty remaining = taker.qty;
    const bool any_price = taker.type == OrderType::Market;

    while (remaining > 0 && !opposite.empty()) {
        auto level_it = opposite.begin();
        const Price level_price = level_it->first;
        if (!any_price && !crosses(taker.side, taker.price, level_price)) {
            break;
        }

        Level& level = level_it->second;
        while (remaining > 0 && !level.queue.empty()) {
            RestingOrder& maker = level.queue.front();
            const Qty qty = std::min(remaining, maker.remaining);

            trades.push_back(Trade{
                .seq = next_trade_seq_++,
                .maker_id = maker.id,
                .taker_id = taker.id,
                .maker_trader = maker.trader,
                .taker_trader = taker.trader,
                .taker_side = taker.side,
                .price = level_price,
                .qty = qty,
            });

            remaining -= qty;
            maker.remaining -= qty;
            level.total -= qty;
            if (maker.remaining == 0) {
                index_.erase(maker.id);
                level.queue.pop_front();
            }
        }

        if (level.queue.empty()) {
            opposite.erase(level_it);
        }
    }
    return remaining;
}

void OrderBook::rest(const NewOrder& order, Qty qty) {
    Level& level = with_book(order.side, [&](auto& book) -> Level& { return book[order.price]; });
    level.queue.push_back(RestingOrder{order.id, order.trader, order.side, order.price, qty});
    level.total += qty;
    index_.emplace(order.id, Locator{order.side, order.price, std::prev(level.queue.end())});
}

void OrderBook::unlink(const Locator& loc) {
    with_book(loc.side, [&](auto& book) {
        auto level_it = book.find(loc.price);
        Level& level = level_it->second;
        level.total -= loc.it->remaining;
        level.queue.erase(loc.it);
        if (level.queue.empty()) {
            book.erase(level_it);
        }
    });
}

bool OrderBook::cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) {
        return false;
    }
    unlink(it->second);
    index_.erase(it);
    return true;
}

SubmitResult OrderBook::modify(OrderId id, Price new_price, Qty new_qty) {
    SubmitResult result;

    auto it = index_.find(id);
    if (it == index_.end()) {
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

    const Locator loc = it->second;
    RestingOrder& order = *loc.it;

    // Pure size reduction: keep the order's place in the queue.
    if (new_price == loc.price && new_qty <= order.remaining) {
        const Qty delta = order.remaining - new_qty;
        with_book(loc.side, [&](auto& book) { book.find(loc.price)->second.total -= delta; });
        order.remaining = new_qty;
        result.resting = new_qty;
        return result;
    }

    // Otherwise cancel/replace: loses priority and may cross the spread.
    const NewOrder replacement{id, order.trader, order.side, OrderType::Limit, new_price, new_qty};
    cancel(id);
    return submit(replacement);
}

std::optional<Price> OrderBook::best_bid() const {
    if (bids_.empty()) {
        return std::nullopt;
    }
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) {
        return std::nullopt;
    }
    return asks_.begin()->first;
}

Qty OrderBook::depth_at(Side side, Price price) const {
    return with_book(side, [&](const auto& book) -> Qty {
        auto it = book.find(price);
        return it == book.end() ? 0 : it->second.total;
    });
}

std::vector<LevelView> OrderBook::levels(Side side, std::size_t max_levels) const {
    std::vector<LevelView> out;
    with_book(side, [&](const auto& book) {
        for (const auto& [price, level] : book) {
            if (out.size() >= max_levels) {
                break;
            }
            out.push_back(LevelView{price, level.total, level.queue.size()});
        }
    });
    return out;
}

const RestingOrder* OrderBook::find(OrderId id) const {
    auto it = index_.find(id);
    return it == index_.end() ? nullptr : &*it->second.it;
}

} // namespace beaglex
