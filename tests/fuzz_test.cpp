// Differential fuzzing: drive the real OrderBook and the naive ReferenceBook
// with the same random operation stream and require identical results after
// every single operation.

#include "beaglex/order_book.hpp"
#include "reference_book.hpp"

#include <gtest/gtest.h>

#include <random>
#include <vector>

using namespace beaglex;

namespace {

void expect_same(const SubmitResult& a, const SubmitResult& b, std::size_t step) {
    ASSERT_EQ(a.reject, b.reject) << "step " << step;
    ASSERT_EQ(a.filled, b.filled) << "step " << step;
    ASSERT_EQ(a.resting, b.resting) << "step " << step;
    ASSERT_EQ(a.cancelled, b.cancelled) << "step " << step;
    ASSERT_EQ(a.trades, b.trades) << "step " << step;
}

void run_fuzz(std::uint64_t seed, std::size_t steps) {
    std::mt19937_64 rng(seed);
    auto uniform = [&](std::int64_t lo, std::int64_t hi) {
        return std::uniform_int_distribution<std::int64_t>(lo, hi)(rng);
    };

    OrderBook book;
    test::ReferenceBook ref;
    std::vector<OrderId> ids; // ids ever submitted, for cancel/modify targets
    OrderId next_id = 1;

    for (std::size_t step = 0; step < steps; ++step) {
        const auto op = uniform(0, 99);

        if (op < 60) {
            // New order around a mid price of 1000 so books cross frequently.
            NewOrder o;
            o.id = uniform(0, 49) == 0 && !ids.empty() ? ids[uniform(0, ids.size() - 1)] : next_id++;
            o.trader = static_cast<TraderId>(uniform(1, 8));
            o.side = uniform(0, 1) ? Side::Buy : Side::Sell;
            const auto t = uniform(0, 9);
            o.type = t < 7 ? OrderType::Limit : t < 9 ? OrderType::ImmediateOrCancel : OrderType::Market;
            o.price = uniform(990, 1010);
            o.qty = uniform(-1, 50);
            ids.push_back(o.id);
            ASSERT_NO_FATAL_FAILURE(expect_same(book.submit(o), ref.submit(o), step));
        } else if (op < 85) {
            const OrderId id = ids.empty() ? 0 : ids[uniform(0, ids.size() - 1)];
            ASSERT_EQ(book.cancel(id), ref.cancel(id)) << "step " << step;
        } else {
            const OrderId id = ids.empty() ? 0 : ids[uniform(0, ids.size() - 1)];
            const Price price = uniform(0, 1) ? uniform(990, 1010) : (book.find(id) ? book.find(id)->price : 1000);
            const Qty qty = uniform(-1, 60);
            ASSERT_NO_FATAL_FAILURE(expect_same(book.modify(id, price, qty), ref.modify(id, price, qty), step));
        }

        ASSERT_EQ(book.order_count(), ref.order_count()) << "step " << step;
        ASSERT_EQ(book.levels(Side::Buy), ref.levels(Side::Buy)) << "step " << step;
        ASSERT_EQ(book.levels(Side::Sell), ref.levels(Side::Sell)) << "step " << step;

        // Invariant: a resting book is never crossed.
        if (book.best_bid() && book.best_ask()) {
            ASSERT_LT(*book.best_bid(), *book.best_ask()) << "step " << step;
        }
    }
}

} // namespace

TEST(Fuzz, MatchesReferenceImplementation) {
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        SCOPED_TRACE(::testing::Message() << "seed " << seed);
        ASSERT_NO_FATAL_FAILURE(run_fuzz(seed, 10'000));
    }
}
