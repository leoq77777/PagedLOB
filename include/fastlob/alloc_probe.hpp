#pragma once

#include <cstddef>

namespace fastlob {

/// Thread-local scoped heap-allocation counter.
/// When built with FASTLOB_ENABLE_ALLOC_PROBE, global operator new increments
/// only while a ScopedAllocProbe is live (avoids counting gtest/gbench setup).
namespace detail {

inline thread_local int g_probe_depth = 0;
inline thread_local std::size_t g_alloc_count = 0;
inline thread_local std::size_t g_alloc_bytes = 0;

inline void note_alloc(std::size_t n) {
    if (g_probe_depth > 0) {
        ++g_alloc_count;
        g_alloc_bytes += n;
    }
}

}  // namespace detail

class ScopedAllocProbe {
public:
    ScopedAllocProbe() {
        if (detail::g_probe_depth == 0) {
            detail::g_alloc_count = 0;
            detail::g_alloc_bytes = 0;
        }
        ++detail::g_probe_depth;
    }

    ~ScopedAllocProbe() { --detail::g_probe_depth; }

    ScopedAllocProbe(const ScopedAllocProbe&) = delete;
    ScopedAllocProbe& operator=(const ScopedAllocProbe&) = delete;

    [[nodiscard]] std::size_t alloc_count() const { return detail::g_alloc_count; }
    [[nodiscard]] std::size_t alloc_bytes() const { return detail::g_alloc_bytes; }
};

}  // namespace fastlob

#define ASSERT_ZERO_ALLOC(probe) \
    ASSERT_EQ((probe).alloc_count(), 0u) << "unexpected heap allocations in hot path"
