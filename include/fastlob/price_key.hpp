#pragma once

#include "fastlob/types.hpp"

#include <cstdint>

namespace fastlob {

/// Sign-bit flip maps signed ticks onto an unsigned order that matches price order.
inline constexpr std::uint64_t encode_tick(Price price) {
    return static_cast<std::uint64_t>(price) ^ (std::uint64_t{1} << 63);
}

/// PageBits is the leaf width in bits: 8 → 256 ticks, 6 → 64, 9 → 512.
inline constexpr int kDefaultPageBits = 8;
inline constexpr std::size_t kTicksPerPage = 256;

template <int PageBits>
inline constexpr std::uint64_t tick_page_key(Price price) {
    static_assert(PageBits >= 5 && PageBits <= 12, "page width must fit a compact occupancy word set");
    return encode_tick(price) >> PageBits;
}

template <int PageBits>
inline constexpr int tick_page_slot(Price price) {
    static_assert(PageBits >= 5 && PageBits <= 12, "page width must fit a compact occupancy word set");
    return static_cast<int>(encode_tick(price) & ((std::uint64_t{1} << PageBits) - 1));
}

template <int PageBits>
inline constexpr Price tick_from_page(std::uint64_t page_key, int slot) {
    static_assert(PageBits >= 5 && PageBits <= 12, "page width must fit a compact occupancy word set");
    const std::uint64_t enc = (page_key << PageBits) | static_cast<std::uint64_t>(slot);
    return static_cast<Price>(enc ^ (std::uint64_t{1} << 63));
}

inline constexpr std::uint64_t tick_page_key(Price price) { return tick_page_key<kDefaultPageBits>(price); }

inline constexpr std::uint8_t tick_page_slot(Price price) {
    return static_cast<std::uint8_t>(tick_page_slot<kDefaultPageBits>(price));
}

inline constexpr Price tick_from_page(std::uint64_t page_key, std::uint8_t slot) {
    return tick_from_page<kDefaultPageBits>(page_key, slot);
}

static_assert(encode_tick(-1) < encode_tick(0));
static_assert(encode_tick(0) < encode_tick(1));
static_assert(encode_tick(255) < encode_tick(256));
static_assert(tick_page_key(0) == tick_page_key(255));
static_assert(tick_page_key(0) != tick_page_key(256));
static_assert(tick_page_key(-1) != tick_page_key(0));
static_assert(tick_page_slot(0) == 0);
static_assert(tick_page_slot(255) == 255);
static_assert(tick_page_slot(256) == 0);
static_assert(tick_page_key<6>(0) == tick_page_key<6>(63));
static_assert(tick_page_key<6>(0) != tick_page_key<6>(64));
static_assert(tick_page_key<9>(0) == tick_page_key<9>(511));
static_assert(tick_page_key<9>(0) != tick_page_key<9>(512));

}  // namespace fastlob
