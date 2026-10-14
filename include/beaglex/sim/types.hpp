#pragma once

#include "beaglex/types.hpp"

#include <cstdint>
#include <optional>

namespace beaglex::sim {

using Time = std::int64_t;  // simulated nanoseconds since the open
using Money = std::int64_t; // 1/1000 of a price tick, so per-share fees of fractions of a cent stay exact
using AgentId = TraderId;

inline constexpr Time kMicrosecond = 1'000;
inline constexpr Time kMillisecond = 1'000'000;
inline constexpr Time kSecond = 1'000'000'000;
inline constexpr Time kMinute = 60 * kSecond;
inline constexpr Time kHour = 60 * kMinute;

inline constexpr Money kMoneyPerTick = 1'000;

// Converts money to dollars, assuming one tick is one cent.
constexpr double to_dollars(Money m) noexcept {
    return static_cast<double>(m) / static_cast<double>(kMoneyPerTick) / 100.0;
}

// Top of book as an agent sees it. Agents never see the live book: the view
// they get is the book as it was one latency ago.
struct MarketView {
    Time as_of = 0;
    std::optional<Price> best_bid;
    std::optional<Price> best_ask;
    Qty bid_size = 0;
    Qty ask_size = 0;
    std::optional<Price> last_trade;

    std::optional<double> mid() const {
        if (best_bid && best_ask) {
            return (static_cast<double>(*best_bid) + static_cast<double>(*best_ask)) / 2.0;
        }
        return std::nullopt;
    }

    // Equality ignores as_of: two views are the same if the market is the same.
    bool same_market(const MarketView& o) const noexcept {
        return best_bid == o.best_bid && best_ask == o.best_ask && bid_size == o.bid_size &&
               ask_size == o.ask_size && last_trade == o.last_trade;
    }
};

// Execution report delivered to an agent (after its latency).
struct Fill {
    OrderId order_id = 0;
    Side side = Side::Buy;
    Price price = 0;
    Qty qty = 0;
    Qty leaves = 0;  // quantity of the order still resting after this fill
    bool maker = false;
    Money fee = 0;   // positive = paid, negative = rebate received
    Time exchange_time = 0;
};

// The exchange's record of an agent's position and cash. Cash starts at zero
// and is unconstrained (no margin model).
struct Account {
    Money cash = 0;
    Qty position = 0;
    Qty volume = 0;
    Money fees = 0;
    std::size_t fills = 0;

    Money mark_to_market(double price_ticks) const noexcept {
        return cash + static_cast<Money>(static_cast<double>(position) * price_ticks * static_cast<double>(kMoneyPerTick));
    }
};

} // namespace beaglex::sim
