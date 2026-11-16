#pragma once

#include "fastlob/types.hpp"

#include <cstdint>

namespace fastlob {

/// Sign-bit flip maps signed ticks onto an unsigned order that matches price order.
/// Page key is the high 56 bits; the low 8 bits select one of 256 ticks on a page.
inline constexpr std::uint64_t encode_tick(Price price) {
    return static_cast<std::uint64_t>(price) ^ (std::uint64_t{1} << 63);
}

inline constexpr std::uint64_t tick_page_key(Price price) { return encode_tick(price) >> 8; }

inline constexpr std::uint8_t tick_page_slot(Price price) {
    return static_cast<std::uint8_t>(encode_tick(price) & 0xFF);
}

inline constexpr Price tick_from_page(std::uint64_t page_key, std::uint8_t slot) {
    const std::uint64_t enc = (page_key << 8) | static_cast<std::uint64_t>(slot);
    return static_cast<Price>(enc ^ (std::uint64_t{1} << 63));
}

inline constexpr std::size_t kTicksPerPage = 256;

static_assert(encode_tick(-1) < encode_tick(0));
static_assert(encode_tick(0) < encode_tick(1));
static_assert(encode_tick(255) < encode_tick(256));
static_assert(tick_page_key(0) == tick_page_key(255));
static_assert(tick_page_key(0) != tick_page_key(256));
static_assert(tick_page_key(-1) != tick_page_key(0));
static_assert(tick_page_slot(0) == 0);
static_assert(tick_page_slot(255) == 255);
static_assert(tick_page_slot(256) == 0);

}  // namespace fastlob
