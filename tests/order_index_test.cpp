#include "beaglex/detail/order_index.hpp"

#include <gtest/gtest.h>

#include <random>
#include <unordered_map>
#include <vector>

using beaglex::OrderId;
using beaglex::detail::OrderIndex;

TEST(OrderIndex, InsertFindErase) {
    OrderIndex idx;
    EXPECT_EQ(idx.find(7), OrderIndex::kNotFound);
    idx.insert(7, 70);
    idx.insert(8, 80);
    EXPECT_EQ(idx.find(7), 70u);
    EXPECT_EQ(idx.find(8), 80u);
    EXPECT_TRUE(idx.erase(7));
    EXPECT_FALSE(idx.erase(7));
    EXPECT_EQ(idx.find(7), OrderIndex::kNotFound);
    EXPECT_EQ(idx.find(8), 80u);
    EXPECT_EQ(idx.size(), 1u);
}

// Differential test against std::unordered_map. A small key space forces long
// probe chains and wrap-around, the cases where backward-shift deletion breaks.
TEST(OrderIndex, MatchesUnorderedMap) {
    for (std::uint64_t seed = 1; seed <= 10; ++seed) {
        std::mt19937_64 rng(seed);
        OrderIndex idx;
        std::unordered_map<OrderId, std::uint32_t> ref;
        const OrderId key_space = seed % 2 ? 64 : 5'000;

        for (int step = 0; step < 50'000; ++step) {
            const OrderId key = rng() % key_space;
            const auto op = rng() % 3;
            if (op == 0 && !ref.contains(key)) {
                const auto value = static_cast<std::uint32_t>(rng() % 1'000'000);
                idx.insert(key, value);
                ref.emplace(key, value);
            } else if (op == 1) {
                ASSERT_EQ(idx.erase(key), ref.erase(key) == 1) << "seed " << seed << " step " << step;
            }
            ASSERT_EQ(idx.size(), ref.size());
            if (step % 97 == 0) {
                for (OrderId k = 0; k < std::min<OrderId>(key_space, 256); ++k) {
                    const auto it = ref.find(k);
                    ASSERT_EQ(idx.find(k), it == ref.end() ? OrderIndex::kNotFound : it->second)
                        << "seed " << seed << " step " << step << " key " << k;
                }
            }
        }
    }
}
