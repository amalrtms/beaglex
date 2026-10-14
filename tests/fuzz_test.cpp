// Differential fuzzing: drive each order book implementation and the naive
// ReferenceBook with the same random operation stream and require identical
// results after every single operation.

#include "beaglex/baseline_order_book.hpp"
#include "beaglex/order_book.hpp"
#include "reference_book.hpp"

#include <gtest/gtest.h>

#include <random>
#include <vector>

using namespace beaglex;

namespace {

struct FuzzConfig {
    std::size_t steps;
    Price mid;
    Price spread;   // prices drawn from [mid - spread, mid + spread]
    int submit_pct; // remaining ops split between cancel and modify
};

template <class Result>
void expect_same(const Result& a, const OwningSubmitResult& b, std::size_t step) {
    ASSERT_EQ(a.reject, b.reject) << "step " << step;
    ASSERT_EQ(a.filled, b.filled) << "step " << step;
    ASSERT_EQ(a.resting, b.resting) << "step " << step;
    ASSERT_EQ(a.cancelled, b.cancelled) << "step " << step;
    ASSERT_EQ(std::vector<Trade>(a.trades.begin(), a.trades.end()), b.trades) << "step " << step;
}

template <class Book>
void run_fuzz(std::uint64_t seed, const FuzzConfig& cfg) {
    std::mt19937_64 rng(seed);
    auto uniform = [&](std::int64_t lo, std::int64_t hi) {
        return std::uniform_int_distribution<std::int64_t>(lo, hi)(rng);
    };
    auto pick = [&](const std::vector<OrderId>& v) {
        return v.empty() ? OrderId{0} : v[static_cast<std::size_t>(uniform(0, static_cast<std::int64_t>(v.size()) - 1))];
    };
    const int cancel_pct = cfg.submit_pct + (100 - cfg.submit_pct) * 2 / 3;

    Book book;
    test::ReferenceBook ref;
    std::vector<OrderId> ids; // ids ever submitted, for cancel/modify targets
    OrderId next_id = 1;

    for (std::size_t step = 0; step < cfg.steps; ++step) {
        const auto op = uniform(0, 99);

        if (op < cfg.submit_pct) {
            NewOrder o;
            o.id = uniform(0, 49) == 0 ? pick(ids) : next_id++; // occasional duplicate id
            o.trader = static_cast<TraderId>(uniform(1, 8));
            o.side = uniform(0, 1) ? Side::Buy : Side::Sell;
            const auto t = uniform(0, 9);
            o.type = t < 7 ? OrderType::Limit : t < 9 ? OrderType::ImmediateOrCancel : OrderType::Market;
            o.price = uniform(cfg.mid - cfg.spread, cfg.mid + cfg.spread);
            o.qty = uniform(-1, 50);
            ids.push_back(o.id);
            ASSERT_NO_FATAL_FAILURE(expect_same(book.submit(o), ref.submit(o), step));
        } else if (op < cancel_pct) {
            const OrderId id = pick(ids);
            ASSERT_EQ(book.cancel(id), ref.cancel(id)) << "step " << step;
        } else {
            const OrderId id = pick(ids);
            const auto resting = book.find(id);
            const Price price = uniform(0, 1) || !resting ? uniform(cfg.mid - cfg.spread, cfg.mid + cfg.spread)
                                                          : resting->price;
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

template <class Book>
class FuzzTest : public ::testing::Test {};

using BookTypes = ::testing::Types<OrderBook, BaselineOrderBook>;
TYPED_TEST_SUITE(FuzzTest, BookTypes);

} // namespace

// Narrow price band: books cross constantly, lots of partial fills.
TYPED_TEST(FuzzTest, DenseCrossingFlow) {
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        SCOPED_TRACE(::testing::Message() << "seed " << seed);
        ASSERT_NO_FATAL_FAILURE(run_fuzz<TypeParam>(seed, {10'000, 1000, 10, 60}));
    }
}

// Wide band, submit-heavy: the book grows to thousands of orders over many
// price levels, exercising container growth and deep-level insert/erase.
TYPED_TEST(FuzzTest, DeepBookGrowth) {
    for (std::uint64_t seed = 100; seed < 104; ++seed) {
        SCOPED_TRACE(::testing::Message() << "seed " << seed);
        ASSERT_NO_FATAL_FAILURE(run_fuzz<TypeParam>(seed, {8'000, 10'000, 400, 85}));
    }
}
