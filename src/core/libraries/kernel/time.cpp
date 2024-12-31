// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <thread>

#include "common/assert.h"
#include "common/native_clock.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/libs.h"

#ifdef _WIN64
#include <windows.h>
#include "common/ntapi.h"
#else
#if __APPLE__
#include <date/tz.h>
#endif
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#endif

namespace Libraries::Kernel {

static u64 initial_ptc;
static std::unique_ptr<Common::NativeClock> clock;

u64 PS4_SYSV_ABI sceKernelGetProcessTime() {
    // TODO: this timer should support suspends, so initial ptc needs to be updated on wake up
    return clock->GetTimeUS(initial_ptc);
}

u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounter() {
    return clock->GetUptime() - initial_ptc;
}

u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounterFrequency() {
    return clock->GetTscFrequency();
}

u64 PS4_SYSV_ABI sceKernelReadTsc() {
    return clock->GetUptime();
}

u64 PS4_SYSV_ABI sceKernelGetTscFrequency() {
    return clock->GetTscFrequency();
}

s32 PS4_SYSV_ABI sceKernelGettimezone(OrbisKernelTimezone* tz) {
#ifdef _WIN64
    ASSERT(tz);
    static int tzflag = 0;
    if (!tzflag) {
        _tzset();
        tzflag++;
    }
    tz->tz_minuteswest = _timezone / 60;
    tz->tz_dsttime = _daylight;
#else
    struct timezone tzz;
    struct timeval tv;
    gettimeofday(&tv, &tzz);
    tz->tz_dsttime = tzz.tz_dsttime;
    tz->tz_minuteswest = tzz.tz_minuteswest;
#endif
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelConvertLocaltimeToUtc(time_t param_1, int64_t param_2, time_t* seconds,
                                                OrbisKernelTimezone* timezone, int* dst_seconds) {
    LOG_INFO(Kernel, "called");
    if (timezone) {
        sceKernelGettimezone(timezone);
        param_1 -= (timezone->tz_minuteswest + timezone->tz_dsttime) * 60;
        if (seconds)
            *seconds = param_1;
        if (dst_seconds)
            *dst_seconds = timezone->tz_dsttime * 60;
    } else {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    return ORBIS_OK;
}

namespace Dev {
u64& GetInitialPtc() {
    return initial_ptc;
}

Common::NativeClock* GetClock() {
    return clock.get();
}

} // namespace Dev

int PS4_SYSV_ABI sceKernelConvertUtcToLocaltime(time_t time, time_t* local_time,
                                                struct OrbisTimesec* st, u64* dst_sec) {
    LOG_TRACE(Kernel, "Called");
#ifdef __APPLE__
    // std::chrono::current_zone() not available yet.
    const auto* time_zone = date::current_zone();
#else
    const auto* time_zone = std::chrono::current_zone();
#endif
    auto info = time_zone->get_info(std::chrono::system_clock::now());

    *local_time = info.offset.count() + info.save.count() * 60 + time;

    if (st != nullptr) {
        st->t = time;
        st->west_sec = info.offset.count() * 60;
        st->dst_sec = info.save.count() * 60;
    }

    if (dst_sec != nullptr) {
        *dst_sec = info.save.count() * 60;
    }

    return ORBIS_OK;
}

int PS4_SYSV_ABI posix_sleep(u32 seconds) {
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    return 0;
}

int PS4_SYSV_ABI posix_usleep(u32 microseconds) {
#ifdef _WIN64
    const auto start_time = std::chrono::high_resolution_clock::now();
    auto total_wait_time = std::chrono::microseconds(microseconds);

    while (total_wait_time.count() > 0) {
        auto wait_time = std::chrono::ceil<std::chrono::milliseconds>(total_wait_time).count();
        u64 res = SleepEx(static_cast<u64>(wait_time), true);
        if (res == WAIT_IO_COMPLETION) {
            auto elapsedTime = std::chrono::high_resolution_clock::now() - start_time;
            auto elapsedMicroseconds =
                std::chrono::duration_cast<std::chrono::microseconds>(elapsedTime).count();
            total_wait_time = std::chrono::microseconds(microseconds - elapsedMicroseconds);
        } else {
            break;
        }
    }
#else
    timespec start;
    timespec remain;
    start.tv_sec = microseconds / 1000000;
    start.tv_nsec = (microseconds % 1000000) * 1000;
    timespec* requested = &start;
    int ret = 0;
    do {
        ret = nanosleep(requested, &remain);
        requested = &remain;
    } while (ret != 0);
#endif
    return 0;
}

int PS4_SYSV_ABI posix_nanosleep(const OrbisKernelTimespec* rqtp, OrbisKernelTimespec* rmtp) {
    if (!rqtp || !rmtp) {
        return POSIX_EFAULT;
    }

    if (rqtp->tv_sec < 0 || rqtp->tv_nsec < 0) {
        return POSIX_EINVAL;
    }

    const auto* request = reinterpret_cast<const timespec*>(rqtp);
    auto* remain = reinterpret_cast<timespec*>(rmtp);
    return nanosleep(request, remain);
}

void DurationToTimespec(auto duration, OrbisKernelTimespec* tp) {
    auto time_s = std::chrono::duration_cast<std::chrono::seconds>(duration);
    auto time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration) -
                   std::chrono::duration_cast<std::chrono::nanoseconds>(time_s);
    tp->tv_sec = time_s.count();
    tp->tv_sec = time_ns.count();
}

int PS4_SYSV_ABI posix_clock_gettime(s32 clock_id, OrbisKernelTimespec* tp) {
    if (tp == nullptr) {
        return POSIX_EFAULT;
    }
    switch (clock_id) {
    default:
        LOG_ERROR(Lib_Kernel, "Unsupported clock ID {}, using realtime.", clock_id);
        [[fallthrough]];
    case ORBIS_CLOCK_REALTIME:
    case ORBIS_CLOCK_REALTIME_PRECISE:
    case ORBIS_CLOCK_REALTIME_FAST:
        DurationToTimespec(std::chrono::system_clock::now().time_since_epoch(), tp);
        break;
    case ORBIS_CLOCK_SECOND:
    case ORBIS_CLOCK_MONOTONIC:
    case ORBIS_CLOCK_MONOTONIC_PRECISE:
    case ORBIS_CLOCK_MONOTONIC_FAST:
        DurationToTimespec(std::chrono::steady_clock::now().time_since_epoch(), tp);
        break;
    }
    return 0;
}

int PS4_SYSV_ABI posix_clock_getres(s32 clock_id, OrbisKernelTimespec* res) {
    if (res == nullptr) {
        return POSIX_EFAULT;
    }
    switch (clock_id) {
    default:
        LOG_ERROR(Lib_Kernel, "Unsupported clock ID {}, using realtime.", clock_id);
        [[fallthrough]];
    case ORBIS_CLOCK_REALTIME:
    case ORBIS_CLOCK_REALTIME_PRECISE:
    case ORBIS_CLOCK_REALTIME_FAST:
        DurationToTimespec(std::chrono::system_clock::duration(1), res);
        break;
    case ORBIS_CLOCK_SECOND:
    case ORBIS_CLOCK_MONOTONIC:
    case ORBIS_CLOCK_MONOTONIC_PRECISE:
    case ORBIS_CLOCK_MONOTONIC_FAST:
        DurationToTimespec(std::chrono::steady_clock::duration(1), res);
        break;
    }
    return 0;
}

int PS4_SYSV_ABI posix_gettimeofday(OrbisKernelTimeval* tp, OrbisKernelTimezone* tz) {
    if (!tp) {
        return POSIX_EFAULT;
    }

#ifdef _WIN64
    FILETIME filetime;
    GetSystemTimePreciseAsFileTime(&filetime);

    constexpr u64 UNIX_TIME_START = 0x295E9648864000;
    constexpr u64 TICKS_PER_SECOND = 1000000;

    u64 ticks = filetime.dwHighDateTime;
    ticks <<= 32;
    ticks |= filetime.dwLowDateTime;
    ticks /= 10;
    ticks -= UNIX_TIME_START;

    tp->tv_sec = ticks / TICKS_PER_SECOND;
    tp->tv_usec = ticks % TICKS_PER_SECOND;
#else
    timeval tv;
    gettimeofday(&tv, nullptr);
    tp->tv_sec = tv.tv_sec;
    tp->tv_usec = tv.tv_usec;
#endif

    // FreeBSD docs mention that the kernel generally does not track these values
    // and they	are usually returned as	zero.
    if (tz) {
        tz->tz_minuteswest = 0;
        tz->tz_dsttime = 0;
    }
    return 0;
}

int PS4_SYSV_ABI sceKernelGettimeofday(OrbisKernelTimeval* tp) {
    auto ret = posix_gettimeofday(tp, nullptr);
    if (ret != 0) {
        ret += ORBIS_KERNEL_ERROR_UNKNOWN;
    }
    return ret;
}

void RegisterTime(Core::Loader::SymbolsResolver* sym) {
    clock = std::make_unique<Common::NativeClock>();
    initial_ptc = clock->GetUptime();
    LIB_FUNCTION("4J2sUJmuHZQ", "libkernel", 1, "libkernel", 1, 1, sceKernelGetProcessTime);
    LIB_FUNCTION("fgxnMeTNUtY", "libkernel", 1, "libkernel", 1, 1, sceKernelGetProcessTimeCounter);
    LIB_FUNCTION("BNowx2l588E", "libkernel", 1, "libkernel", 1, 1,
                 sceKernelGetProcessTimeCounterFrequency);
    LIB_FUNCTION("-2IRUCO--PM", "libkernel", 1, "libkernel", 1, 1, sceKernelReadTsc);
    LIB_FUNCTION("1j3S3n-tTW4", "libkernel", 1, "libkernel", 1, 1, sceKernelGetTscFrequency);
    LIB_FUNCTION("kOcnerypnQA", "libkernel", 1, "libkernel", 1, 1, sceKernelGettimezone);
    LIB_FUNCTION("0NTHN1NKONI", "libkernel", 1, "libkernel", 1, 1, sceKernelConvertLocaltimeToUtc);
    LIB_FUNCTION("-o5uEDpN+oY", "libkernel", 1, "libkernel", 1, 1, sceKernelConvertUtcToLocaltime);

    LIB_FUNCTION("ejekcaNQNq0", "libkernel", 1, "libkernel", 1, 1, sceKernelGettimeofday);
    LIB_FUNCTION("n88vx3C5nW8", "libkernel", 1, "libkernel", 1, 1, posix_gettimeofday);
    LIB_FUNCTION("n88vx3C5nW8", "libScePosix", 1, "libkernel", 1, 1, posix_gettimeofday);
    LIB_FUNCTION("-ZR+hG7aDHw", "libkernel", 1, "libkernel", 1, 1, ORBIS(posix_sleep));
    LIB_FUNCTION("0wu33hunNdE", "libkernel", 1, "libkernel", 1, 1, posix_sleep);
    LIB_FUNCTION("0wu33hunNdE", "libScePosix", 1, "libkernel", 1, 1, posix_sleep);
    LIB_FUNCTION("1jfXLRVzisc", "libkernel", 1, "libkernel", 1, 1, ORBIS(posix_usleep));
    LIB_FUNCTION("QcteRwbsnV0", "libkernel", 1, "libkernel", 1, 1, posix_usleep);
    LIB_FUNCTION("QcteRwbsnV0", "libScePosix", 1, "libkernel", 1, 1, posix_usleep);
    LIB_FUNCTION("QvsZxomvUHs", "libkernel", 1, "libkernel", 1, 1, ORBIS(posix_nanosleep));
    LIB_FUNCTION("yS8U2TGCe1A", "libkernel", 1, "libkernel", 1, 1, posix_nanosleep);
    LIB_FUNCTION("yS8U2TGCe1A", "libScePosix", 1, "libkernel", 1, 1, posix_nanosleep);
    LIB_FUNCTION("QBi7HCK03hw", "libkernel", 1, "libkernel", 1, 1, ORBIS(posix_clock_gettime));
    LIB_FUNCTION("lLMT9vJAck0", "libkernel", 1, "libkernel", 1, 1, posix_clock_gettime);
    LIB_FUNCTION("lLMT9vJAck0", "libScePosix", 1, "libkernel", 1, 1, posix_clock_gettime);
    LIB_FUNCTION("wRYVA5Zolso", "libkernel", 1, "libkernel", 1, 1, ORBIS(posix_clock_getres));
    LIB_FUNCTION("smIj7eqzZE8", "libkernel", 1, "libkernel", 1, 1, posix_clock_getres);
    LIB_FUNCTION("smIj7eqzZE8", "libScePosix", 1, "libkernel", 1, 1, posix_clock_getres);
}

} // namespace Libraries::Kernel
