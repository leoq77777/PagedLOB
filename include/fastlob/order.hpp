#pragma once

#include "fastlob/types.hpp"

#include <cstddef>

namespace fastlob {

/// Compact order node. Intrusive prev/next live on the object for Fast LOB FIFO.
/// Intentionally NOT alignas(64): prefer cache density; false-sharing isolation
/// belongs at pool/index boundaries, not on every Order.
struct Order {
    OrderId order_id{kInvalidOrderId};
    Price price{0};
    Qty qty{0};
    Side side{Side::Buy};
    std::uint8_t pad_[3]{};
    Order* prev{nullptr};
    Order* next{nullptr};
    Timestamp timestamp{0};
};

static_assert(sizeof(Order) <= 64, "Order must fit in a single cache line");
static_assert(alignof(Order) <= 8, "Order must not force cache-line alignment");

}  // namespace fastlob
