// Walks through the worked example: a buy order that sweeps two ask levels.

#include "beaglex/order_book.hpp"

#include <cstdio>

using namespace beaglex;

namespace {

void print_book(const OrderBook& book) {
    const auto asks = book.levels(Side::Sell, 5);
    const auto bids = book.levels(Side::Buy, 5);

    std::printf("  %-10s %8s %7s\n", "", "price", "qty");
    for (auto it = asks.rbegin(); it != asks.rend(); ++it) {
        std::printf("  %-10s %8s %7lld  (%zu orders)\n", "ASK", format_price(it->price).c_str(),
                    static_cast<long long>(it->qty), it->orders);
    }
    std::printf("  ---------- spread ----------\n");
    for (const auto& level : bids) {
        std::printf("  %-10s %8s %7lld  (%zu orders)\n", "BID", format_price(level.price).c_str(),
                    static_cast<long long>(level.qty), level.orders);
    }
    std::printf("\n");
}

void submit(OrderBook& book, const NewOrder& order) {
    const auto result = book.submit(order);
    std::printf("> #%llu %s %s %lld @ %s\n", static_cast<unsigned long long>(order.id),
                to_string(order.type), to_string(order.side), static_cast<long long>(order.qty),
                order.type == OrderType::Market ? "any price" : format_price(order.price).c_str());
    if (!result.accepted()) {
        std::printf("  rejected: %s\n", to_string(result.reject));
        return;
    }
    for (const auto& t : result.trades) {
        std::printf("  TRADE %lld @ %s  (maker #%llu, taker #%llu)\n", static_cast<long long>(t.qty),
                    format_price(t.price).c_str(), static_cast<unsigned long long>(t.maker_id),
                    static_cast<unsigned long long>(t.taker_id));
    }
    std::printf("  filled %lld, resting %lld, cancelled %lld\n\n", static_cast<long long>(result.filled),
                static_cast<long long>(result.resting), static_cast<long long>(result.cancelled));
}

} // namespace

int main() {
    OrderBook book;

    // Prices in cents.
    const NewOrder initial[] = {
        {1, 100, Side::Sell, OrderType::Limit, 15050, 300},
        {2, 101, Side::Buy, OrderType::Limit, 14995, 500},
        {3, 102, Side::Sell, OrderType::Limit, 15010, 200},
        {5, 103, Side::Sell, OrderType::Limit, 15020, 100},
        {6, 104, Side::Sell, OrderType::Limit, 15050, 100},
        {7, 105, Side::Buy, OrderType::Limit, 15000, 100},
        {8, 106, Side::Buy, OrderType::Limit, 14990, 150},
        {9, 107, Side::Buy, OrderType::Limit, 15000, 200},
    };
    for (const auto& order : initial) {
        book.submit(order);
    }

    std::printf("Initial book:\n");
    print_book(book);

    submit(book, {11, 200, Side::Buy, OrderType::Limit, 15020, 250});
    std::printf("After the sweep:\n");
    print_book(book);

    submit(book, {12, 201, Side::Sell, OrderType::Market, 0, 350});
    std::printf("After a market sell:\n");
    print_book(book);
    return 0;
}
