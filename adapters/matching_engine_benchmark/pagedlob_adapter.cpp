/*
 * pagedlob_adapter.cpp — matching_engine_api.h adapter backed by fastlob::PagedLOB.
 *
 * Benchmark suite: https://github.com/flash1-dev/matching-engine-benchmark
 *
 * Zero external shadow state:
 * - Order liveness, FIFO queue priority, price-level matching, IOC execution,
 *   and resting metadata (`cancel_ex`) are handled 100% natively inside
 *   `fastlob::PagedLOB` (`OrderIdIndex` + `OrderPool` + `LevelPool` +
 *   zero-migration unified `PagedPriceIndex`).
 * - Fills stream inline via `match_limit_cb` / `match_ioc_cb` directly into the
 *   harness SPSC transport with zero heap allocations and zero vector buffering.
 * - Audit queries (`engine_query_best_bid`, `engine_query_best_ask`,
 *   `engine_query_depth_at`) query `PagedLOB` directly.
 */

#include "matching_engine_api.h"
#include "fastlob/storage_book.hpp"

#include <climits>
#include <cstdint>

#if defined(__aarch64__)
static inline void cpu_pause() { asm volatile("yield" ::: "memory"); }
#elif defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
static inline void cpu_pause() { _mm_pause(); }
#else
static inline void cpu_pause() {}
#endif

namespace {

// Sized above maximum concurrent live resting orders across all 5 canonical scenarios
// (`static` peaks around ~42k resting orders; `normal`/`swing`/`flash-crash` < 2k),
// keeping `OrderIdIndex` (2.1 MiB) + `OrderPool` (3.1 MiB) + `LevelPool` (0.8 MiB)
// comfortably inside L3 cache.
constexpr std::size_t kMaxRestingOrders = 1u << 16;  // 65,536 concurrent resting orders
constexpr std::size_t kMaxActiveLevels  = 1u << 14;  // 16,384 concurrent active price levels

fastlob::PagedLOB*    g_book      = nullptr;
const me_transport_t* g_transport = nullptr;
void*                 g_sink      = nullptr;

inline void push_report(const me_report_t& r) {
    while (!g_transport->push(g_sink, &r)) {
        cpu_pause();
    }
}

inline void emit_ack(uint8_t type, uint64_t seq, uint64_t order_id,
                     uint8_t side, int64_t price, uint32_t qty) {
    me_report_t r{};
    r.type            = type;
    r.side            = side;
    r.sequence_number = seq;
    r.order_id        = order_id;
    r.price_ticks     = price;
    r.quantity        = qty;
    push_report(r);
}

inline void emit_trade(uint64_t seq, const fastlob::Fill& f) {
    me_report_t r{};
    r.type            = ME_TRADE;
    r.sequence_number = seq;
    r.order_id        = f.maker_id;
    r.price_ticks     = f.price;
    r.quantity        = f.qty;
    r.maker_order_id  = f.maker_id;
    r.taker_order_id  = f.taker_id;
    push_report(r);
}

}  // namespace

extern "C" {

void engine_init(uint64_t /*seed*/, const me_transport_t* transport,
                 void* report_sink) {
    g_transport = transport;
    g_sink      = report_sink;
    g_book      = new fastlob::PagedLOB(kMaxRestingOrders, kMaxActiveLevels);
    g_book->reserve_cold(64, 256);
}

void engine_shutdown(void) {
    delete g_book;
    g_book = nullptr;
}

void engine_flush(void) {}

void engine_on_new_order(const new_order_t* o) {
    const uint64_t seq = o->sequence_number;
    emit_ack(ME_ORDER_ACK, seq, o->order_id, o->side, o->price_ticks, o->quantity);

    const fastlob::Side side = (o->side == 0) ? fastlob::Side::Buy : fastlob::Side::Sell;

    if (o->ioc) {
        const fastlob::MatchResult res = g_book->match_ioc_cb(
            o->order_id, side, o->price_ticks, o->quantity,
            [seq](const fastlob::Fill& f) { emit_trade(seq, f); });
        if (res.remaining > 0) {
            emit_ack(ME_CANCEL_ACK, seq, o->order_id, o->side, o->price_ticks, res.remaining);
        }
        return;
    }

    g_book->match_limit_cb(
        o->order_id, side, o->price_ticks, o->quantity,
        [seq](const fastlob::Fill& f) { emit_trade(seq, f); });
}

void engine_on_cancel(const cancel_t* c) {
    const fastlob::CancelInfo info = g_book->cancel_ex(c->order_id);
    if (info.ok) {
        const uint8_t side = (info.side == fastlob::Side::Buy) ? 0 : 1;
        emit_ack(ME_CANCEL_ACK, c->sequence_number, c->order_id, side, info.price, 0);
    } else {
        emit_ack(ME_CANCEL_REJECT, c->sequence_number, c->order_id, 0, 0, 0);
    }
}

void engine_on_modify(const modify_t* m) {
    if (!g_book->cancel(m->order_id)) {
        emit_ack(ME_MODIFY_REJECT, m->sequence_number, m->order_id, 0, 0, 0);
        return;
    }
    const uint64_t seq = m->sequence_number;
    const fastlob::Side side = (m->side == 0) ? fastlob::Side::Buy : fastlob::Side::Sell;
    g_book->match_limit_cb(
        m->order_id, side, m->new_price_ticks, m->new_quantity,
        [seq](const fastlob::Fill& f) { emit_trade(seq, f); });
    emit_ack(ME_MODIFY_ACK, seq, m->order_id, m->side, m->new_price_ticks, m->new_quantity);
}

int64_t engine_query_best_bid(void) {
    const auto p = g_book->best_price(fastlob::Side::Buy);
    return p.has_value() ? *p : INT64_MIN;
}

int64_t engine_query_best_ask(void) {
    const auto p = g_book->best_price(fastlob::Side::Sell);
    return p.has_value() ? *p : INT64_MAX;
}

uint64_t engine_query_depth_at(int64_t price_ticks, uint8_t side) {
    return g_book->level_qty((side == 0) ? fastlob::Side::Buy : fastlob::Side::Sell,
                             price_ticks);
}

}  // extern "C"
