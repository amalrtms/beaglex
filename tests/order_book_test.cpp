#include "beaglex/order_book.hpp"

#include <gtest/gtest.h>

using namespace beaglex;

namespace {

NewOrder limit(OrderId id, Side side, Price price, Qty qty, TraderId trader = 1) {
    return {id, trader, side, OrderType::Limit, price, qty};
}

NewOrder market(OrderId id, Side side, Qty qty, TraderId trader = 1) {
    return {id, trader, side, OrderType::Market, 0, qty};
}

NewOrder ioc(OrderId id, Side side, Price price, Qty qty, TraderId trader = 1) {
    return {id, trader, side, OrderType::ImmediateOrCancel, price, qty};
}

} // namespace

TEST(OrderBook, EmptyBookHasNoTopOfBook) {
    OrderBook book;
    EXPECT_FALSE(book.best_bid());
    EXPECT_FALSE(book.best_ask());
    EXPECT_EQ(book.order_count(), 0u);
}

TEST(OrderBook, NonCrossingOrdersRest) {
    OrderBook book;
    auto r1 = book.submit(limit(1, Side::Buy, 100, 10));
    auto r2 = book.submit(limit(2, Side::Sell, 101, 5));

    EXPECT_TRUE(r1.trades.empty());
    EXPECT_EQ(r1.resting, 10);
    EXPECT_EQ(r2.resting, 5);
    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_EQ(book.best_ask(), 101);
    EXPECT_EQ(book.order_count(), 2u);
}

TEST(OrderBook, TradesAtMakerPrice) {
    OrderBook book;
    book.submit(limit(1, Side::Sell, 100, 10));
    auto r = book.submit(limit(2, Side::Buy, 105, 10));

    ASSERT_EQ(r.trades.size(), 1u);
    EXPECT_EQ(r.trades[0].price, 100);
    EXPECT_EQ(r.trades[0].maker_id, 1u);
    EXPECT_EQ(r.trades[0].taker_id, 2u);
    EXPECT_EQ(r.filled, 10);
    EXPECT_EQ(book.order_count(), 0u);
}

TEST(OrderBook, PricePriorityBeatsTimePriority) {
    OrderBook book;
    book.submit(limit(1, Side::Sell, 102, 10));
    book.submit(limit(2, Side::Sell, 101, 10)); // later but cheaper

    auto r = book.submit(limit(3, Side::Buy, 102, 10));
    ASSERT_EQ(r.trades.size(), 1u);
    EXPECT_EQ(r.trades[0].maker_id, 2u);
    EXPECT_EQ(r.trades[0].price, 101);
}

TEST(OrderBook, TimePriorityWithinLevel) {
    OrderBook book;
    book.submit(limit(1, Side::Buy, 100, 5));
    book.submit(limit(2, Side::Buy, 100, 5));
    book.submit(limit(3, Side::Buy, 100, 5));

    auto r = book.submit(limit(4, Side::Sell, 100, 7));
    ASSERT_EQ(r.trades.size(), 2u);
    EXPECT_EQ(r.trades[0].maker_id, 1u);
    EXPECT_EQ(r.trades[0].qty, 5);
    EXPECT_EQ(r.trades[1].maker_id, 2u);
    EXPECT_EQ(r.trades[1].qty, 2);
    EXPECT_EQ(book.find(2)->remaining, 3);
    EXPECT_EQ(book.depth_at(Side::Buy, 100), 8);
}

TEST(OrderBook, WorkedExampleSweepsTwoLevels) {
    OrderBook book;
    book.submit(limit(3, Side::Sell, 15010, 200));
    book.submit(limit(5, Side::Sell, 15020, 100));
    book.submit(limit(1, Side::Sell, 15050, 300));

    auto r = book.submit(limit(11, Side::Buy, 15020, 250));
    ASSERT_EQ(r.trades.size(), 2u);
    EXPECT_EQ(r.trades[0].qty, 200);
    EXPECT_EQ(r.trades[0].price, 15010);
    EXPECT_EQ(r.trades[1].qty, 50);
    EXPECT_EQ(r.trades[1].price, 15020);
    EXPECT_EQ(r.filled, 250);
    EXPECT_EQ(r.resting, 0);
    EXPECT_EQ(book.best_ask(), 15020);
    EXPECT_EQ(book.depth_at(Side::Sell, 15020), 50);
}

TEST(OrderBook, LimitRemainderRestsAfterSweep) {
    OrderBook book;
    book.submit(limit(1, Side::Sell, 100, 10));
    auto r = book.submit(limit(2, Side::Buy, 100, 25));

    EXPECT_EQ(r.filled, 10);
    EXPECT_EQ(r.resting, 15);
    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_FALSE(book.best_ask());
}

TEST(OrderBook, MarketOrderWalksBookAndNeverRests) {
    OrderBook book;
    book.submit(limit(1, Side::Buy, 100, 10));
    book.submit(limit(2, Side::Buy, 99, 10));

    auto r = book.submit(market(3, Side::Sell, 25));
    ASSERT_EQ(r.trades.size(), 2u);
    EXPECT_EQ(r.filled, 20);
    EXPECT_EQ(r.cancelled, 5);
    EXPECT_EQ(r.resting, 0);
    EXPECT_FALSE(book.best_ask());
    EXPECT_EQ(book.order_count(), 0u);
}

TEST(OrderBook, MarketOrderOnEmptyBookIsCancelled) {
    OrderBook book;
    auto r = book.submit(market(1, Side::Buy, 10));
    EXPECT_TRUE(r.accepted());
    EXPECT_EQ(r.filled, 0);
    EXPECT_EQ(r.cancelled, 10);
}

TEST(OrderBook, IocRespectsLimitAndDiscardsRemainder) {
    OrderBook book;
    book.submit(limit(1, Side::Sell, 100, 5));
    book.submit(limit(2, Side::Sell, 102, 5));

    auto r = book.submit(ioc(3, Side::Buy, 101, 20));
    EXPECT_EQ(r.filled, 5);
    EXPECT_EQ(r.cancelled, 15);
    EXPECT_EQ(book.best_ask(), 102);
    EXPECT_FALSE(book.best_bid());
}

TEST(OrderBook, CancelRemovesOrderAndEmptyLevel) {
    OrderBook book;
    book.submit(limit(1, Side::Buy, 100, 10));
    book.submit(limit(2, Side::Buy, 99, 10));

    EXPECT_TRUE(book.cancel(1));
    EXPECT_FALSE(book.cancel(1));
    EXPECT_EQ(book.best_bid(), 99);
    EXPECT_EQ(book.depth_at(Side::Buy, 100), 0);
    EXPECT_EQ(book.find(1), nullptr);
}

TEST(OrderBook, CancelledOrderNoLongerMatches) {
    OrderBook book;
    book.submit(limit(1, Side::Sell, 100, 10));
    book.cancel(1);
    auto r = book.submit(limit(2, Side::Buy, 100, 10));
    EXPECT_TRUE(r.trades.empty());
}

TEST(OrderBook, RejectsInvalidOrders) {
    OrderBook book;
    EXPECT_EQ(book.submit(limit(1, Side::Buy, 100, 0)).reject, RejectReason::InvalidQuantity);
    EXPECT_EQ(book.submit(limit(1, Side::Buy, 100, -5)).reject, RejectReason::InvalidQuantity);
    EXPECT_EQ(book.submit(limit(1, Side::Buy, 0, 5)).reject, RejectReason::InvalidPrice);

    book.submit(limit(1, Side::Buy, 100, 5));
    EXPECT_EQ(book.submit(limit(1, Side::Sell, 200, 5)).reject, RejectReason::DuplicateOrderId);
    EXPECT_EQ(book.order_count(), 1u);
}

TEST(OrderBook, ModifyReduceKeepsPriority) {
    OrderBook book;
    book.submit(limit(1, Side::Buy, 100, 10));
    book.submit(limit(2, Side::Buy, 100, 10));

    auto m = book.modify(1, 100, 4);
    EXPECT_TRUE(m.accepted());
    EXPECT_EQ(book.depth_at(Side::Buy, 100), 14);

    auto r = book.submit(limit(3, Side::Sell, 100, 4));
    ASSERT_EQ(r.trades.size(), 1u);
    EXPECT_EQ(r.trades[0].maker_id, 1u); // still first in line
}

TEST(OrderBook, ModifyIncreaseLosesPriority) {
    OrderBook book;
    book.submit(limit(1, Side::Buy, 100, 10));
    book.submit(limit(2, Side::Buy, 100, 10));

    book.modify(1, 100, 20);

    auto r = book.submit(limit(3, Side::Sell, 100, 10));
    ASSERT_EQ(r.trades.size(), 1u);
    EXPECT_EQ(r.trades[0].maker_id, 2u);
}

TEST(OrderBook, ModifyPriceCanCross) {
    OrderBook book;
    book.submit(limit(1, Side::Sell, 105, 10));
    book.submit(limit(2, Side::Buy, 100, 10));

    auto r = book.modify(2, 105, 10);
    ASSERT_EQ(r.trades.size(), 1u);
    EXPECT_EQ(r.trades[0].price, 105);
    EXPECT_EQ(book.order_count(), 0u);
}

TEST(OrderBook, ModifyUnknownOrderIsRejected) {
    OrderBook book;
    EXPECT_EQ(book.modify(42, 100, 10).reject, RejectReason::UnknownOrder);
}

TEST(OrderBook, TradeSequenceNumbersIncrease) {
    OrderBook book;
    book.submit(limit(1, Side::Sell, 100, 5));
    book.submit(limit(2, Side::Sell, 100, 5));
    auto r1 = book.submit(limit(3, Side::Buy, 100, 10));
    book.submit(limit(4, Side::Sell, 100, 5));
    auto r2 = book.submit(limit(5, Side::Buy, 100, 5));

    ASSERT_EQ(r1.trades.size(), 2u);
    ASSERT_EQ(r2.trades.size(), 1u);
    EXPECT_EQ(r1.trades[0].seq, 1u);
    EXPECT_EQ(r1.trades[1].seq, 2u);
    EXPECT_EQ(r2.trades[0].seq, 3u);
}

TEST(FormatPrice, FormatsTicks) {
    EXPECT_EQ(format_price(15025), "150.25");
    EXPECT_EQ(format_price(5), "0.05");
    EXPECT_EQ(format_price(-150), "-1.50");
    EXPECT_EQ(format_price(7, 0), "7");
}
