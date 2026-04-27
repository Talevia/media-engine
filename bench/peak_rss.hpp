/*
 * me::bench::peak_rss_bytes — query the high-water-mark resident set
 * size of the current process, in bytes.
 *
 * VISION §5.7-3: "单次渲染的内存峰值是否在现有 examples 里被观察"
 * lived as an open question because nothing measured peak RSS. This
 * header is the measurement point — bench programs read it pre/post
 * the workload and dump the delta. The mach (macOS) and getrusage
 * (Linux) paths agree on units (bytes), so callers can compare values
 * across platforms without per-OS scaling.
 *
 * Why peak RSS, not "current RSS": cache-heavy workloads (frame pool,
 * codec ctx pool) churn allocations so current RSS at sample time
 * understates real footprint. ru_maxrss / TaskInfo.resident_size_max
 * are monotonic-rising — they capture the worst moment during the
 * workload window even if that moment happened before sampling.
 *
 * Why header-only: bench/ has no compilation unit conventions and
 * each bench program is its own translation unit. Inlining keeps the
 * dependency graph trivial.
 *
 * Returns 0 on platforms / failures we don't handle, so callers MUST
 * check for non-zero before treating the value as a budget input.
 */
#pragma once

#include <cstdint>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <sys/resource.h>
#include <sys/time.h>
#endif

namespace me::bench {

inline std::int64_t peak_rss_bytes() {
#if defined(__APPLE__)
    /* mach_task_basic_info exposes resident_size_max — the true
     * high-water mark since process start. task_basic_info (the
     * older flavor) only reports current resident_size, which would
     * miss transient peaks. */
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        return static_cast<std::int64_t>(info.resident_size_max);
    }
    return 0;
#elif defined(__linux__)
    /* getrusage ru_maxrss is in KILOBYTES on Linux (POSIX leaves the
     * unit unspecified; Linux chose KB, BSD/macOS chose bytes — we
     * normalise to bytes here so callers see one unit). */
    struct rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        return static_cast<std::int64_t>(ru.ru_maxrss) * 1024;
    }
    return 0;
#else
    return 0;
#endif
}

}  // namespace me::bench
