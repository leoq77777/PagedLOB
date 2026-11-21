#pragma once

#include <cstdint>
#include <limits>

namespace fastlob {

using OrderId = std::uint64_t;
using Price = std::int64_t;     // fixed-point ticks
using Qty = std::uint32_t;      // per-order quantity
using LevelQty = std::uint64_t; // aggregate quantity on a price level
using Timestamp = std::uint64_t;

/// Tick size: 1 tick = 0.01 currency units (cents).
inline constexpr Price kTickSizeFactor = 100;

enum class Side : std::uint8_t {
    Buy = 0,
    Sell = 1,
};

enum class EventType : std::uint8_t {
    Add = 0,
    Cancel = 1,
    Modify = 2,   // qty decrease only in MVP; price change = Cancel+Add at feed level
    Execute = 3,  // trade / partial or full fill of a resting order
};

struct BookEvent {
    EventType type{EventType::Add};
    OrderId order_id{0};
    Side side{Side::Buy};
    Price price{0};
    Qty qty{0};
    Timestamp ts{0};
};

struct LevelView {
    Price price{0};
    LevelQty qty{0};
};

struct Fill {
    OrderId maker_id{0};
    OrderId taker_id{0};
    Price price{0};
    Qty qty{0};
};

struct MatchResult {
    bool ok{false};
    Qty filled{0};
    Qty remaining{0};
    bool rested{false};
};

inline constexpr OrderId kInvalidOrderId = 0;
inline constexpr Price kInvalidPrice = std::numeric_limits<Price>::min();

constexpr Price dollars_to_ticks(double dollars) {
    return static_cast<Price>(dollars * static_cast<double>(kTickSizeFactor) + (dollars >= 0 ? 0.5 : -0.5));
}

constexpr double ticks_to_dollars(Price ticks) {
    return static_cast<double>(ticks) / static_cast<double>(kTickSizeFactor);
}

}  // namespace fastlob
