#pragma once

#include "beaglex/types.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace beaglex::detail {

// Open-addressing hash map from OrderId to a pool index.
//
// All slots live in one flat array (no per-entry heap node), probing is
// linear so a lookup usually touches a single cache line, and erase uses
// backward-shift deletion so no tombstones accumulate under heavy cancel
// traffic. Load factor is kept at or below 1/2.
class OrderIndex {
public:
    using Value = std::uint32_t;
    static constexpr Value kNotFound = UINT32_MAX;

    OrderIndex() { reset(16); }

    void reserve(std::size_t n) {
        const std::size_t want = std::bit_ceil(std::max<std::size_t>(16, n * 2));
        if (want > slots_.size()) {
            rehash(want);
        }
    }

    std::size_t size() const noexcept { return size_; }

    Value find(OrderId key) const noexcept {
        for (std::size_t i = home(key);; i = (i + 1) & mask_) {
            const Slot& s = slots_[i];
            if (s.value == kNotFound) {
                return kNotFound;
            }
            if (s.key == key) {
                return s.value;
            }
        }
    }

    bool contains(OrderId key) const noexcept { return find(key) != kNotFound; }

    // Precondition: key is not present.
    void insert(OrderId key, Value value) {
        if ((size_ + 1) * 2 > slots_.size()) {
            rehash(slots_.size() * 2);
        }
        std::size_t i = home(key);
        while (slots_[i].value != kNotFound) {
            i = (i + 1) & mask_;
        }
        slots_[i] = {key, value};
        ++size_;
    }

    bool erase(OrderId key) noexcept {
        std::size_t i = home(key);
        while (true) {
            if (slots_[i].value == kNotFound) {
                return false;
            }
            if (slots_[i].key == key) {
                break;
            }
            i = (i + 1) & mask_;
        }

        // Backward-shift: pull later entries of the probe chain into the hole
        // so every remaining key stays reachable from its home slot.
        std::size_t hole = i;
        for (std::size_t j = (hole + 1) & mask_; slots_[j].value != kNotFound; j = (j + 1) & mask_) {
            const std::size_t h = home(slots_[j].key);
            // Entry j may move to `hole` only if its home is not in the
            // cyclic range (hole, j]; otherwise moving it would put it before
            // its home slot and make it unreachable.
            const bool home_in_range = hole <= j ? (hole < h && h <= j) : (hole < h || h <= j);
            if (!home_in_range) {
                slots_[hole] = slots_[j];
                hole = j;
            }
        }
        slots_[hole].value = kNotFound;
        --size_;
        return true;
    }

private:
    struct Slot {
        OrderId key;
        Value value; // kNotFound marks an empty slot
    };

    // splitmix64 finaliser: sequential ids would otherwise cluster badly.
    static std::uint64_t mix(std::uint64_t x) noexcept {
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return x;
    }

    std::size_t home(OrderId key) const noexcept { return static_cast<std::size_t>(mix(key)) & mask_; }

    void reset(std::size_t capacity) {
        slots_.assign(capacity, Slot{0, kNotFound});
        mask_ = capacity - 1;
        size_ = 0;
    }

    void rehash(std::size_t capacity) {
        std::vector<Slot> old = std::move(slots_);
        reset(capacity);
        for (const Slot& s : old) {
            if (s.value != kNotFound) {
                insert(s.key, s.value);
            }
        }
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    std::size_t size_ = 0;
};

} // namespace beaglex::detail
