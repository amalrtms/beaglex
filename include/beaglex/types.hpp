#pragma once

#include <cstdint>
#include <string>

namespace beaglex {

// Prices are integer ticks (e.g. cents), never floating point: binary floats
// cannot represent most decimal prices exactly, which breaks equality and
// ordering of price levels. Real exchanges do the same.
using Price = std::int64_t;
using Qty = std::int64_t;
using OrderId = std::uint64_t;
using TraderId = std::uint32_t;
using SeqNum = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };

enum class OrderType : std::uint8_t {
    Limit,             // match what crosses, rest the remainder in the book
    Market,            // match at any price, discard the remainder
    ImmediateOrCancel, // match up to the limit price, discard the remainder
};

enum class RejectReason : std::uint8_t {
    None,
    InvalidQuantity,
    InvalidPrice,
    DuplicateOrderId,
    UnknownOrder,
};

constexpr Side opposite(Side s) noexcept { return s == Side::Buy ? Side::Sell : Side::Buy; }

struct NewOrder {
    OrderId id = 0;
    TraderId trader = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    Price price = 0; // ignored for Market orders
    Qty qty = 0;
};

// A single execution between a resting (maker) order and an incoming (taker) order.
// Always priced at the maker's price.
struct Trade {
    SeqNum seq = 0;
    OrderId maker_id = 0;
    OrderId taker_id = 0;
    TraderId maker_trader = 0;
    TraderId taker_trader = 0;
    Side taker_side = Side::Buy;
    Price price = 0;
    Qty qty = 0;

    bool operator==(const Trade&) const = default;
};

const char* to_string(Side s) noexcept;
const char* to_string(OrderType t) noexcept;
const char* to_string(RejectReason r) noexcept;

// Formats a tick price with a fixed number of decimals, e.g. 15025 -> "150.25".
std::string format_price(Price ticks, int decimals = 2);

} // namespace beaglex
