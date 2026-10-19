#pragma once

#include "fastlob/types.hpp"

#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace fastlob {

/// Compact binary event record (fixed 32 bytes) for synthetic MBO replay.
#pragma pack(push, 1)
struct BinaryEvent {
    std::uint8_t type;  // EventType
    std::uint8_t side;  // Side
    std::uint16_t reserved{0};
    OrderId order_id;
    Price price;
    Qty qty;
    Timestamp ts;
};
#pragma pack(pop)

static_assert(sizeof(BinaryEvent) == 32, "BinaryEvent size");

inline BinaryEvent to_binary(const BookEvent& e) {
    BinaryEvent b{};
    b.type = static_cast<std::uint8_t>(e.type);
    b.side = static_cast<std::uint8_t>(e.side);
    b.order_id = e.order_id;
    b.price = e.price;
    b.qty = e.qty;
    b.ts = e.ts;
    return b;
}

inline BookEvent from_binary(const BinaryEvent& b) {
    BookEvent e;
    e.type = static_cast<EventType>(b.type);
    e.side = static_cast<Side>(b.side);
    e.order_id = b.order_id;
    e.price = b.price;
    e.qty = b.qty;
    e.ts = b.ts;
    return e;
}

inline bool write_events_bin(const std::string& path, const std::vector<BookEvent>& events) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    for (const auto& e : events) {
        BinaryEvent b = to_binary(e);
        out.write(reinterpret_cast<const char*>(&b), sizeof(b));
    }
    return static_cast<bool>(out);
}

inline bool read_events_bin(const std::string& path, std::vector<BookEvent>& events) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    events.clear();
    BinaryEvent b{};
    while (in.read(reinterpret_cast<char*>(&b), sizeof(b))) {
        events.push_back(from_binary(b));
    }
    return true;
}

/// Minimal CSV: type,order_id,side,price_ticks,qty,ts
/// type: A|C|M|E  side: B|S
inline bool write_events_csv(const std::string& path, const std::vector<BookEvent>& events) {
    std::ofstream out(path);
    if (!out) {
        return false;
    }
    out << "type,order_id,side,price_ticks,qty,ts\n";
    for (const auto& e : events) {
        char t = '?';
        switch (e.type) {
            case EventType::Add:
                t = 'A';
                break;
            case EventType::Cancel:
                t = 'C';
                break;
            case EventType::Modify:
                t = 'M';
                break;
            case EventType::Execute:
                t = 'E';
                break;
        }
        out << t << ',' << e.order_id << ',' << (e.side == Side::Buy ? 'B' : 'S') << ','
            << e.price << ',' << e.qty << ',' << e.ts << '\n';
    }
    return static_cast<bool>(out);
}

inline bool read_events_csv(const std::string& path, std::vector<BookEvent>& events) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    events.clear();
    std::string line;
    std::getline(in, line);  // header
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        BookEvent e{};
        char t = 0;
        char s = 0;
        // sscanf-style parse
        std::size_t p0 = 0;
        auto next = [&](std::size_t from) {
            auto c = line.find(',', from);
            return c == std::string::npos ? line.size() : c;
        };
        std::size_t p1 = next(p0);
        t = line[p0];
        p0 = p1 + 1;
        p1 = next(p0);
        e.order_id = std::stoull(line.substr(p0, p1 - p0));
        p0 = p1 + 1;
        p1 = next(p0);
        s = line[p0];
        p0 = p1 + 1;
        p1 = next(p0);
        e.price = std::stoll(line.substr(p0, p1 - p0));
        p0 = p1 + 1;
        p1 = next(p0);
        e.qty = static_cast<Qty>(std::stoul(line.substr(p0, p1 - p0)));
        p0 = p1 + 1;
        e.ts = std::stoull(line.substr(p0));

        switch (t) {
            case 'A':
                e.type = EventType::Add;
                break;
            case 'C':
                e.type = EventType::Cancel;
                break;
            case 'M':
                e.type = EventType::Modify;
                break;
            case 'E':
                e.type = EventType::Execute;
                break;
            default:
                return false;
        }
        e.side = (s == 'B') ? Side::Buy : Side::Sell;
        events.push_back(e);
    }
    return true;
}

template <typename Book>
std::size_t replay_into(Book& book, const std::vector<BookEvent>& events) {
    std::size_t applied = 0;
    for (const auto& e : events) {
        if (book.apply(e)) {
            ++applied;
        }
    }
    return applied;
}

}  // namespace fastlob
