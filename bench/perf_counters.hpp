#pragma once

#include <cstdint>
#include <cstring>

#if defined(__linux__)
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace fastlob::bench {

struct PerfSnapshot {
    bool available{false};
    std::uint64_t cycles{0};
    std::uint64_t instructions{0};
    std::uint64_t branch_misses{0};
    std::uint64_t l1d_load_misses{0};
    std::uint64_t llc_load_misses{0};

    [[nodiscard]] double ipc() const noexcept {
        return cycles > 0 ? static_cast<double>(instructions) / static_cast<double>(cycles) : 0.0;
    }
};

/// Scoped wrapper around Linux `perf_event_open` so benchmarks can collect hardware PMU
/// counters around ONLY the hot loop (excluding file load and book setup) on bare-metal Linux / WSL.
/// Gracefully reports `available = false` when PMU counters are virtualized away (e.g. cloud KVM).
class LinuxPerfCounters {
public:
    LinuxPerfCounters() {
#if defined(__linux__)
        fd_cycles_ = open_hw(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, -1);
        if (fd_cycles_ < 0) {
            return;
        }
        fd_inst_ = open_hw(PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS, fd_cycles_);
        fd_brmiss_ = open_hw(PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES, fd_cycles_);
        fd_l1d_ = open_cache(PERF_COUNT_HW_CACHE_L1D, PERF_COUNT_HW_CACHE_OP_READ,
                             PERF_COUNT_HW_CACHE_RESULT_MISS, fd_cycles_);
        fd_llc_ = open_cache(PERF_COUNT_HW_CACHE_LL, PERF_COUNT_HW_CACHE_OP_READ,
                             PERF_COUNT_HW_CACHE_RESULT_MISS, fd_cycles_);
        available_ = (fd_inst_ >= 0);
#endif
    }

    ~LinuxPerfCounters() {
#if defined(__linux__)
        for (int fd : {fd_llc_, fd_l1d_, fd_brmiss_, fd_inst_, fd_cycles_}) {
            if (fd >= 0) {
                ::close(fd);
            }
        }
#endif
    }

    LinuxPerfCounters(const LinuxPerfCounters&) = delete;
    LinuxPerfCounters& operator=(const LinuxPerfCounters&) = delete;

    [[nodiscard]] bool available() const noexcept { return available_; }

    void reset_and_enable() noexcept {
#if defined(__linux__)
        if (!available_) {
            return;
        }
        for (int fd : {fd_cycles_, fd_inst_, fd_brmiss_, fd_l1d_, fd_llc_}) {
            if (fd >= 0) {
                ::ioctl(fd, PERF_EVENT_IOC_RESET, 0);
                ::ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
            }
        }
#endif
    }

    void disable() noexcept {
#if defined(__linux__)
        if (!available_) {
            return;
        }
        for (int fd : {fd_cycles_, fd_inst_, fd_brmiss_, fd_l1d_, fd_llc_}) {
            if (fd >= 0) {
                ::ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
            }
        }
#endif
    }

    [[nodiscard]] PerfSnapshot read() const noexcept {
        PerfSnapshot s{};
#if defined(__linux__)
        if (!available_) {
            return s;
        }
        s.available = true;
        s.cycles = read_one(fd_cycles_);
        s.instructions = read_one(fd_inst_);
        s.branch_misses = read_one(fd_brmiss_);
        s.l1d_load_misses = read_one(fd_l1d_);
        s.llc_load_misses = read_one(fd_llc_);
#endif
        return s;
    }

private:
#if defined(__linux__)
    static int open_Attr(perf_event_attr& attr, int group_fd) noexcept {
        return static_cast<int>(::syscall(SYS_perf_event_open, &attr, 0, -1, group_fd, 0));
    }

    static int open_hw(std::uint32_t type, std::uint64_t config, int group_fd) noexcept {
        perf_event_attr attr{};
        attr.type = type;
        attr.size = sizeof(attr);
        attr.config = config;
        attr.disabled = 1;
        attr.exclude_kernel = 1;
        attr.exclude_hv = 1;
        // Open standalone if group leader runs out of multiplex slots on hybrid/WSL PMUs
        int fd = open_Attr(attr, group_fd);
        if (fd < 0 && group_fd >= 0) {
            fd = open_Attr(attr, -1);
        }
        return fd;
    }

    static int open_cache(std::uint64_t cache_id, std::uint64_t op_id,
                          std::uint64_t result_id, int group_fd) noexcept {
        const std::uint64_t cfg = cache_id | (op_id << 8) | (result_id << 16);
        return open_hw(PERF_TYPE_HW_CACHE, cfg, group_fd);
    }

    static std::uint64_t read_one(int fd) noexcept {
        if (fd < 0) {
            return 0;
        }
        std::uint64_t val = 0;
        if (::read(fd, &val, sizeof(val)) != sizeof(val)) {
            return 0;
        }
        return val;
    }

    int fd_cycles_{-1};
    int fd_inst_{-1};
    int fd_brmiss_{-1};
    int fd_l1d_{-1};
    int fd_llc_{-1};
#endif
    bool available_{false};
};

}  // namespace fastlob::bench
