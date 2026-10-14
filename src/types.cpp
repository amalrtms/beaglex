#include "beaglex/types.hpp"

#include <cstdlib>

namespace beaglex {

const char* to_string(Side s) noexcept {
    return s == Side::Buy ? "BUY" : "SELL";
}

const char* to_string(OrderType t) noexcept {
    switch (t) {
    case OrderType::Limit: return "LIMIT";
    case OrderType::Market: return "MARKET";
    case OrderType::ImmediateOrCancel: return "IOC";
    }
    return "?";
}

const char* to_string(RejectReason r) noexcept {
    switch (r) {
    case RejectReason::None: return "None";
    case RejectReason::InvalidQuantity: return "InvalidQuantity";
    case RejectReason::InvalidPrice: return "InvalidPrice";
    case RejectReason::DuplicateOrderId: return "DuplicateOrderId";
    case RejectReason::UnknownOrder: return "UnknownOrder";
    }
    return "?";
}

std::string format_price(Price ticks, int decimals) {
    Price scale = 1;
    for (int i = 0; i < decimals; ++i) {
        scale *= 10;
    }
    const bool negative = ticks < 0;
    const Price abs_ticks = negative ? -ticks : ticks;

    std::string out = negative ? "-" : "";
    out += std::to_string(abs_ticks / scale);
    if (decimals > 0) {
        std::string frac = std::to_string(abs_ticks % scale);
        out += '.';
        out.append(static_cast<std::size_t>(decimals) - frac.size(), '0');
        out += frac;
    }
    return out;
}

} // namespace beaglex
