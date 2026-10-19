#include "fastlob/alloc_probe.hpp"

#include <cstdlib>
#include <new>

#if defined(FASTLOB_ENABLE_ALLOC_PROBE)

void* operator new(std::size_t n) {
    fastlob::detail::note_alloc(n);
    if (void* p = std::malloc(n)) {
        return p;
    }
    throw std::bad_alloc{};
}

void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    fastlob::detail::note_alloc(n);
    return std::malloc(n);
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }

void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new[](std::size_t n, const std::nothrow_t& nt) noexcept {
    return ::operator new(n, nt);
}
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t n) noexcept { ::operator delete(p, n); }

#endif
