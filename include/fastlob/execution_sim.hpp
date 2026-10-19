#pragma once

#include "fastlob/lob_fast.hpp"
#include "fastlob/types.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace fastlob {

enum class OrderType : std::uint8_t { Limit = 0, Market = 1 };

struct FillReport {
    OrderId strategy_order_id{0};
    Price price{0};
    Qty qty{0};
    Timestamp ts{0};
    bool fully_filled{false};
};

struct ShadowOrder {
    OrderId id{0};
    Side side{Side::Buy};
    Price price{0};
    Qty qty{0};
    Qty remaining{0};
    Qty ahead_qty{0};
    bool active{false};
};

/// Strategy execution against a read-only public book + trade stream.
/// Shadow limit orders never enter the public FIFO.
class ExecutionSimulator {
public:
    explicit ExecutionSimulator(const FastOrderBook& book) : book_(book) {}

    /// Place shadow limit; records ahead_qty from public level total.
    bool submit_limit(OrderId id, Side side, Price price, Qty qty) {
        if (id == kInvalidOrderId || qty == 0 || find_shadow(id) != nullptr) {
            return false;
        }
        ShadowOrder s;
        s.id = id;
        s.side = side;
        s.price = price;
        s.qty = qty;
        s.remaining = qty;
        s.ahead_qty = book_.level_qty(side, price);
        s.active = true;
        shadows_.push_back(s);
        return true;
    }

    bool cancel_shadow(OrderId id) {
        ShadowOrder* s = find_shadow(id);
        if (s == nullptr || !s->active) {
            return false;
        }
        s->active = false;
        return true;
    }

    /// Market order: walk opposing depth for VWAP / slippage (does not mutate book).
    std::optional<FillReport> submit_market(OrderId id, Side side, Qty qty, Timestamp ts = 0) {
        if (qty == 0) {
            return std::nullopt;
        }
        const Side book_side = (side == Side::Buy) ? Side::Sell : Side::Buy;
        LevelView levels[64];
        const std::size_t n = book_.top_n(book_side, levels);
        Qty left = qty;
        std::int64_t notional_ticks = 0;
        Qty filled = 0;
        for (std::size_t i = 0; i < n && left > 0; ++i) {
            const Qty take = std::min(left, levels[i].qty);
            notional_ticks += static_cast<std::int64_t>(take) * levels[i].price;
            filled += take;
            left -= take;
        }
        if (filled == 0) {
            return std::nullopt;
        }
        FillReport fr;
        fr.strategy_order_id = id;
        fr.qty = filled;
        fr.price = static_cast<Price>(notional_ticks / static_cast<std::int64_t>(filled));
        fr.ts = ts;
        fr.fully_filled = (filled == qty);
        last_fills_.push_back(fr);
        return fr;
    }

    /// Observe a public trade at `price` for `qty` on the trade's aggressor-consumed side.
    /// For queue tracking: trades that lift offers consume ask-side ahead; hits consume bids.
    /// `resting_side` is the side of the book that was resting and got executed.
    void on_public_trade(Side resting_side, Price price, Qty qty, Timestamp ts = 0) {
        for (auto& s : shadows_) {
            if (!s.active || s.remaining == 0) {
                continue;
            }
            if (s.side != resting_side || s.price != price) {
                continue;
            }
            Qty trade_left = qty;
            if (s.ahead_qty > 0) {
                const Qty consume = std::min(s.ahead_qty, trade_left);
                s.ahead_qty -= consume;
                trade_left -= consume;
            }
            if (trade_left > 0 && s.ahead_qty == 0) {
                const Qty fill_qty = std::min(s.remaining, trade_left);
                s.remaining -= fill_qty;
                FillReport fr;
                fr.strategy_order_id = s.id;
                fr.price = price;
                fr.qty = fill_qty;
                fr.ts = ts;
                fr.fully_filled = (s.remaining == 0);
                last_fills_.push_back(fr);
                if (s.remaining == 0) {
                    s.active = false;
                }
            }
        }
    }

    /// Convenience: after applying Execute to the public book, notify sim with same params.
    void on_book_execute(OrderId /*public_id*/, Side resting_side, Price price, Qty exec_qty,
                         Timestamp ts = 0) {
        on_public_trade(resting_side, price, exec_qty, ts);
    }

    [[nodiscard]] const ShadowOrder* shadow(OrderId id) const { return find_shadow_c(id); }

    [[nodiscard]] bool shadow_in_public_book(OrderId id) const {
        return book_.find(id).has_value();
    }

    [[nodiscard]] const std::vector<FillReport>& fills() const { return last_fills_; }

    void clear_fills() { last_fills_.clear(); }

private:
    ShadowOrder* find_shadow(OrderId id) {
        for (auto& s : shadows_) {
            if (s.id == id) {
                return &s;
            }
        }
        return nullptr;
    }

    const ShadowOrder* find_shadow_c(OrderId id) const {
        for (const auto& s : shadows_) {
            if (s.id == id) {
                return &s;
            }
        }
        return nullptr;
    }

    const FastOrderBook& book_;
    std::vector<ShadowOrder> shadows_;
    std::vector<FillReport> last_fills_;
};

}  // namespace fastlob
