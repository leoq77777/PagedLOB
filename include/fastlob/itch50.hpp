#pragma once

#include "fastlob/types.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#if defined(FASTLOB_HAS_ZLIB)
#include <zlib.h>
#endif

namespace fastlob::itch50 {

inline std::uint16_t read_be16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) |
                                      static_cast<std::uint16_t>(p[1]));
}

inline std::uint32_t read_be32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

inline std::uint64_t read_be48(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint64_t>(p[0]) << 40) |
           (static_cast<std::uint64_t>(p[1]) << 32) |
           (static_cast<std::uint64_t>(p[2]) << 24) |
           (static_cast<std::uint64_t>(p[3]) << 16) |
           (static_cast<std::uint64_t>(p[4]) << 8) |
           static_cast<std::uint64_t>(p[5]);
}

inline std::uint64_t read_be64(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint64_t>(read_be32(p)) << 32) |
           static_cast<std::uint64_t>(read_be32(p + 4));
}

inline void write_be16(std::uint8_t* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v);
}

inline void write_be32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}

inline void write_be48(std::uint8_t* p, std::uint64_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v >> 40);
    p[1] = static_cast<std::uint8_t>(v >> 32);
    p[2] = static_cast<std::uint8_t>(v >> 24);
    p[3] = static_cast<std::uint8_t>(v >> 16);
    p[4] = static_cast<std::uint8_t>(v >> 8);
    p[5] = static_cast<std::uint8_t>(v);
}

inline void write_be64(std::uint8_t* p, std::uint64_t v) noexcept {
    write_be32(p, static_cast<std::uint32_t>(v >> 32));
    write_be32(p + 4, static_cast<std::uint32_t>(v));
}

inline std::string_view trim_stock_symbol(const char* raw8) noexcept {
    std::size_t end = 8;
    while (end > 0 && (raw8[end - 1] == ' ' || raw8[end - 1] == '\0')) {
        --end;
    }
    return std::string_view(raw8, end);
}

struct ItchStats {
    std::uint64_t total_wire_msgs{0};
    std::uint64_t symbol_wire_msgs{0};
    std::uint64_t book_events{0};
    std::uint64_t add_msgs{0};          // 'A' + 'F'
    std::uint64_t cancel_part_msgs{0};  // 'X'
    std::uint64_t delete_msgs{0};       // 'D'
    std::uint64_t replace_msgs{0};      // 'U' (expands to Cancel + Add)
    std::uint64_t execute_msgs{0};      // 'E' + 'C'
    std::uint64_t subpenny_msgs{0};     // price4 % tick_divisor != 0
    std::uint64_t unknown_ref_skips{0};
    std::uint16_t stock_locate{0};
    bool locate_found{false};
    Timestamp market_open_ts{0};   // System Event 'Q' (09:30 EST)
    Timestamp market_close_ts{0};  // System Event 'M' (16:00 EST)
};

/// Stateful single-symbol converter from NASDAQ TotalView-ITCH 5.0 wire messages to `BookEvent`s.
/// Handles ITCH 5.0 nuances:
/// - Wire prices have 4 implied decimals ($0.0001). `tick_divisor = 100` maps Reg NMS $0.01 ticks
///   to consecutive integers matching `fastlob::kTickSizeFactor` (100 ticks/$1); pass `1` for raw 1e-4 ticks.
/// - `'X'` (Order Cancel) subtracts `cancelled_shares` -> `Modify(new_qty)` or `Cancel` when 0;
/// - `'D'` (Order Delete) -> `Cancel`;
/// - `'U'` (Order Replace) inherits `Side` from `orig_order_ref` -> `Cancel(orig)` + `Add(new)`.
class ItchSymbolNormalizer {
public:
    explicit ItchSymbolNormalizer(std::string target_symbol, std::uint32_t tick_divisor = 100,
                                  std::uint16_t explicit_locate = 0)
        : target_symbol_(std::move(target_symbol)),
          tick_divisor_(tick_divisor == 0 ? 1 : tick_divisor) {
        live_.reserve(1 << 17);
        if (explicit_locate != 0) {
            stats_.stock_locate = explicit_locate;
            stats_.locate_found = true;
        }
    }

    template <typename EmitFn>
    void on_message(const std::uint8_t* msg, std::size_t len, EmitFn&& emit) {
        if (len < 11) {
            return;
        }
        ++stats_.total_wire_msgs;
        const char type = static_cast<char>(msg[0]);
        const std::uint16_t locate = read_be16(msg + 1);

        if (type == 'S') {
            if (len >= 12) {
                const Timestamp ts = read_be48(msg + 5);
                const char code = static_cast<char>(msg[11]);
                if (code == 'Q') {
                    stats_.market_open_ts = ts;
                } else if (code == 'M') {
                    stats_.market_close_ts = ts;
                }
            }
            return;
        }

        if (type == 'R') {
            if (len >= 19 && !stats_.locate_found) {
                const auto sym = trim_stock_symbol(reinterpret_cast<const char*>(msg + 11));
                if (sym == target_symbol_) {
                    stats_.stock_locate = locate;
                    stats_.locate_found = true;
                }
            }
            return;
        }

        if (!stats_.locate_found || locate != stats_.stock_locate) {
            return;
        }
        ++stats_.symbol_wire_msgs;
        const Timestamp ts = read_be48(msg + 5);

        switch (type) {
            case 'A':
            case 'F': {
                if ((type == 'A' && len < 36) || (type == 'F' && len < 40)) {
                    return;
                }
                const OrderId ref = read_be64(msg + 11);
                const Side side = (msg[19] == 'B') ? Side::Buy : Side::Sell;
                const Qty shares = read_be32(msg + 20);
                const std::uint32_t price4 = read_be32(msg + 32);
                if (price4 % tick_divisor_ != 0) {
                    ++stats_.subpenny_msgs;
                }
                const Price price = static_cast<Price>(price4 / tick_divisor_);
                if (ref == kInvalidOrderId || shares == 0) {
                    return;
                }
                live_[ref] = LiveOrder{side, shares};
                ++stats_.add_msgs;
                ++stats_.book_events;
                emit(BookEvent{EventType::Add, ref, side, price, shares, ts});
                break;
            }
            case 'X': {
                if (len < 23) {
                    return;
                }
                const OrderId ref = read_be64(msg + 11);
                const Qty cancelled = read_be32(msg + 19);
                auto it = live_.find(ref);
                if (it == live_.end()) {
                    ++stats_.unknown_ref_skips;
                    return;
                }
                ++stats_.cancel_part_msgs;
                ++stats_.book_events;
                const Side side = it->second.side;
                if (cancelled >= it->second.qty) {
                    live_.erase(it);
                    emit(BookEvent{EventType::Cancel, ref, side, 0, 0, ts});
                } else {
                    it->second.qty -= cancelled;
                    emit(BookEvent{EventType::Modify, ref, side, 0, it->second.qty, ts});
                }
                break;
            }
            case 'D': {
                if (len < 19) {
                    return;
                }
                const OrderId ref = read_be64(msg + 11);
                auto it = live_.find(ref);
                if (it == live_.end()) {
                    ++stats_.unknown_ref_skips;
                    return;
                }
                const Side side = it->second.side;
                live_.erase(it);
                ++stats_.delete_msgs;
                ++stats_.book_events;
                emit(BookEvent{EventType::Cancel, ref, side, 0, 0, ts});
                break;
            }
            case 'E':
            case 'C': {
                if ((type == 'E' && len < 31) || (type == 'C' && len < 36)) {
                    return;
                }
                const OrderId ref = read_be64(msg + 11);
                const Qty executed = read_be32(msg + 19);
                auto it = live_.find(ref);
                if (it == live_.end()) {
                    ++stats_.unknown_ref_skips;
                    return;
                }
                const Side side = it->second.side;
                const Qty exec_clamped = (executed > it->second.qty) ? it->second.qty : executed;
                if (exec_clamped >= it->second.qty) {
                    live_.erase(it);
                } else {
                    it->second.qty -= exec_clamped;
                }
                ++stats_.execute_msgs;
                ++stats_.book_events;
                emit(BookEvent{EventType::Execute, ref, side, 0, exec_clamped, ts});
                break;
            }
            case 'U': {
                if (len < 35) {
                    return;
                }
                const OrderId orig_ref = read_be64(msg + 11);
                const OrderId new_ref = read_be64(msg + 19);
                const Qty shares = read_be32(msg + 27);
                const std::uint32_t price4 = read_be32(msg + 31);
                if (price4 % tick_divisor_ != 0) {
                    ++stats_.subpenny_msgs;
                }
                const Price price = static_cast<Price>(price4 / tick_divisor_);
                auto it = live_.find(orig_ref);
                if (it == live_.end()) {
                    ++stats_.unknown_ref_skips;
                    return;
                }
                const Side side = it->second.side;
                live_.erase(it);
                ++stats_.replace_msgs;
                ++stats_.book_events;
                emit(BookEvent{EventType::Cancel, orig_ref, side, 0, 0, ts});
                if (new_ref != kInvalidOrderId && shares > 0) {
                    live_[new_ref] = LiveOrder{side, shares};
                    ++stats_.book_events;
                    emit(BookEvent{EventType::Add, new_ref, side, price, shares, ts});
                }
                break;
            }
            default:
                break;
        }
    }

    [[nodiscard]] const ItchStats& stats() const noexcept { return stats_; }

private:
    struct LiveOrder {
        Side side{Side::Buy};
        Qty qty{0};
    };

    std::string target_symbol_;
    std::uint32_t tick_divisor_{100};
    ItchStats stats_{};
    std::unordered_map<OrderId, LiveOrder> live_;
};

/// Stream length-prefixed `[uint16_be len][payload]` ITCH 5.0 frames from a memory buffer.
template <typename MsgFn>
std::size_t parse_itch50_buffer(const std::uint8_t* data, std::size_t size, MsgFn&& on_msg) {
    std::size_t offset = 0;
    std::size_t count = 0;
    while (offset + 2 <= size) {
        const std::uint16_t len = read_be16(data + offset);
        if (len == 0 || offset + 2 + len > size) {
            break;
        }
        on_msg(data + offset + 2, len);
        offset += 2 + static_cast<std::size_t>(len);
        ++count;
    }
    return count;
}

/// Stream an uncompressed or `.gz` (when `FASTLOB_HAS_ZLIB` is defined) ITCH 5.0 file.
template <typename MsgFn>
bool stream_itch50_file(const std::string& path, MsgFn&& on_msg, std::size_t max_msgs = 0) {
    std::FILE* probe = std::fopen(path.c_str(), "rb");
    if (probe == nullptr) {
        return false;
    }
    unsigned char magic[2]{};
    const std::size_t n_magic = std::fread(magic, 1, 2, probe);
    std::fclose(probe);
    const bool is_gzip = (n_magic == 2 && magic[0] == 0x1f && magic[1] == 0x8b);

    constexpr std::size_t kBufSize = 4u << 20;  // 4 MiB streaming window
    std::vector<std::uint8_t> buf(kBufSize + 65536);
    std::size_t carry = 0;
    std::size_t msgs = 0;

    if (is_gzip) {
#if defined(FASTLOB_HAS_ZLIB)
        gzFile gz = gzopen(path.c_str(), "rb");
        if (gz == nullptr) {
            return false;
        }
        gzbuffer(gz, 1u << 20);
        while (true) {
            const int got = gzread(gz, buf.data() + carry, static_cast<unsigned>(kBufSize));
            if (got < 0) {
                gzclose(gz);
                return false;
            }
            const std::size_t total = carry + static_cast<std::size_t>(got);
            std::size_t offset = 0;
            while (offset + 2 <= total) {
                const std::uint16_t len = read_be16(buf.data() + offset);
                if (len == 0) {
                    gzclose(gz);
                    return false;
                }
                if (offset + 2 + len > total) {
                    break;
                }
                on_msg(buf.data() + offset + 2, len);
                offset += 2 + static_cast<std::size_t>(len);
                ++msgs;
                if (max_msgs != 0 && msgs >= max_msgs) {
                    gzclose(gz);
                    return true;
                }
            }
            carry = total - offset;
            if (carry > 0 && offset > 0) {
                std::memmove(buf.data(), buf.data() + offset, carry);
            }
            if (got == 0) {
                break;
            }
        }
        gzclose(gz);
        return carry == 0;
#else
        return false;
#endif
    }

    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (fp == nullptr) {
        return false;
    }
    while (true) {
        const std::size_t got = std::fread(buf.data() + carry, 1, kBufSize, fp);
        const std::size_t total = carry + got;
        std::size_t offset = 0;
        while (offset + 2 <= total) {
            const std::uint16_t len = read_be16(buf.data() + offset);
            if (len == 0) {
                std::fclose(fp);
                return false;
            }
            if (offset + 2 + len > total) {
                break;
            }
            on_msg(buf.data() + offset + 2, len);
            offset += 2 + static_cast<std::size_t>(len);
            ++msgs;
            if (max_msgs != 0 && msgs >= max_msgs) {
                std::fclose(fp);
                return true;
            }
        }
        carry = total - offset;
        if (carry > 0 && offset > 0) {
            std::memmove(buf.data(), buf.data() + offset, carry);
        }
        if (got == 0) {
            break;
        }
    }
    std::fclose(fp);
    return carry == 0;
}

// ---- Wire frame builders for unit & differential testing ----

inline void append_framed(std::vector<std::uint8_t>& out, const std::uint8_t* payload, std::uint16_t len) {
    std::uint8_t hdr[2];
    write_be16(hdr, len);
    out.insert(out.end(), hdr, hdr + 2);
    out.insert(out.end(), payload, payload + len);
}

inline void write_stock_padded(std::uint8_t* dst, std::string_view sym) noexcept {
    std::memset(dst, ' ', 8);
    const std::size_t n = sym.size() < 8 ? sym.size() : 8;
    std::memcpy(dst, sym.data(), n);
}

inline void encode_stock_directory(std::vector<std::uint8_t>& out, std::uint16_t locate,
                                   std::string_view sym, Timestamp ts = 1) {
    std::uint8_t msg[39]{};
    msg[0] = 'R';
    write_be16(msg + 1, locate);
    write_be48(msg + 5, ts);
    write_stock_padded(msg + 11, sym);
    append_framed(out, msg, sizeof(msg));
}

inline void encode_system_event(std::vector<std::uint8_t>& out, char code, Timestamp ts) {
    std::uint8_t msg[12]{};
    msg[0] = 'S';
    write_be48(msg + 5, ts);
    msg[11] = static_cast<std::uint8_t>(code);
    append_framed(out, msg, sizeof(msg));
}

inline void encode_add_order(std::vector<std::uint8_t>& out, std::uint16_t locate, Timestamp ts,
                             OrderId ref, Side side, Qty shares, std::string_view sym,
                             std::uint32_t price4) {
    std::uint8_t msg[36]{};
    msg[0] = 'A';
    write_be16(msg + 1, locate);
    write_be48(msg + 5, ts);
    write_be64(msg + 11, ref);
    msg[19] = (side == Side::Buy) ? 'B' : 'S';
    write_be32(msg + 20, shares);
    write_stock_padded(msg + 24, sym);
    write_be32(msg + 32, price4);
    append_framed(out, msg, sizeof(msg));
}

inline void encode_order_cancel(std::vector<std::uint8_t>& out, std::uint16_t locate, Timestamp ts,
                                OrderId ref, Qty cancelled_shares) {
    std::uint8_t msg[23]{};
    msg[0] = 'X';
    write_be16(msg + 1, locate);
    write_be48(msg + 5, ts);
    write_be64(msg + 11, ref);
    write_be32(msg + 19, cancelled_shares);
    append_framed(out, msg, sizeof(msg));
}

inline void encode_order_delete(std::vector<std::uint8_t>& out, std::uint16_t locate, Timestamp ts,
                                OrderId ref) {
    std::uint8_t msg[19]{};
    msg[0] = 'D';
    write_be16(msg + 1, locate);
    write_be48(msg + 5, ts);
    write_be64(msg + 11, ref);
    append_framed(out, msg, sizeof(msg));
}

inline void encode_order_executed(std::vector<std::uint8_t>& out, std::uint16_t locate, Timestamp ts,
                                  OrderId ref, Qty executed_shares, std::uint64_t match_no = 1) {
    std::uint8_t msg[31]{};
    msg[0] = 'E';
    write_be16(msg + 1, locate);
    write_be48(msg + 5, ts);
    write_be64(msg + 11, ref);
    write_be32(msg + 19, executed_shares);
    write_be64(msg + 23, match_no);
    append_framed(out, msg, sizeof(msg));
}

inline void encode_order_replace(std::vector<std::uint8_t>& out, std::uint16_t locate, Timestamp ts,
                                 OrderId orig_ref, OrderId new_ref, Qty shares,
                                 std::uint32_t price4) {
    std::uint8_t msg[35]{};
    msg[0] = 'U';
    write_be16(msg + 1, locate);
    write_be48(msg + 5, ts);
    write_be64(msg + 11, orig_ref);
    write_be64(msg + 19, new_ref);
    write_be32(msg + 27, shares);
    write_be32(msg + 31, price4);
    append_framed(out, msg, sizeof(msg));
}

}  // namespace fastlob::itch50
