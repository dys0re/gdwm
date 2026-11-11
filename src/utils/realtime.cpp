/*
    SPDX-FileCopyrightText: 2022 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "realtime.h"

#include "config-kwin.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>

namespace KWin
{

void gainRealTime(int targetPriority)
{
    int maxPrio = sched_get_priority_max(SCHED_FIFO);
    int minPrio = sched_get_priority_min(SCHED_FIFO);
    if (maxPrio < 0 || minPrio < 0) {
        maxPrio = 99;
        minPrio = 1;
    }

    struct rlimit rlimRt;
    if (getrlimit(RLIMIT_RTPRIO, &rlimRt) == 0 && rlimRt.rlim_cur != RLIM_INFINITY) {
        if (static_cast<rlim_t>(maxPrio) > rlimRt.rlim_cur) {
            maxPrio = static_cast<int>(rlimRt.rlim_cur);
        }
    }

    int ret = -1;
    if (maxPrio >= minPrio) {
        const int prio = std::clamp(targetPriority, minPrio, maxPrio);
        sched_param sp{};
        sp.sched_priority = prio;

#if defined(HAVE_SCHED_RESET_ON_FORK) && HAVE_SCHED_RESET_ON_FORK
        ret = pthread_setschedparam(pthread_self(), SCHED_FIFO | SCHED_RESET_ON_FORK, &sp);
        if (ret != 0)
#endif
        {
            ret = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        }
    }

    if (ret != 0) {
        // Fallback to SCHED_RR
        int rrMax = sched_get_priority_max(SCHED_RR);
        int rrMin = sched_get_priority_min(SCHED_RR);
        if (rrMax < 0 || rrMin < 0) {
            rrMax = 99;
            rrMin = 1;
        }
        if (getrlimit(RLIMIT_RTPRIO, &rlimRt) == 0 && rlimRt.rlim_cur != RLIM_INFINITY) {
            if (static_cast<rlim_t>(rrMax) > rlimRt.rlim_cur) {
                rrMax = static_cast<int>(rlimRt.rlim_cur);
            }
        }
        if (rrMax >= rrMin) {
            const int prio = std::clamp(targetPriority, rrMin, rrMax);
            sched_param sp{};
            sp.sched_priority = prio;
#if defined(HAVE_SCHED_RESET_ON_FORK) && HAVE_SCHED_RESET_ON_FORK
            ret = pthread_setschedparam(pthread_self(), SCHED_RR | SCHED_RESET_ON_FORK, &sp);
            if (ret != 0)
#endif
            {
                ret = pthread_setschedparam(pthread_self(), SCHED_RR, &sp);
            }
        }
    }

    if (ret != 0) {
        // Fallback to nice -15 if realtime scheduling is denied
        setpriority(PRIO_PROCESS, 0, -15);
    }
}

void triggerInputBoost()
{
    using clock = std::chrono::steady_clock;
    static std::atomic<int64_t> s_lastBoostMs{0};

    const auto now = clock::now();
    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    int64_t lastMs = s_lastBoostMs.load(std::memory_order_relaxed);

    if (nowMs - lastMs < 500) {
        return; // Coalesced: within ~500ms cooldown
    }

    if (!s_lastBoostMs.compare_exchange_strong(lastMs, nowMs, std::memory_order_relaxed)) {
        return; // Another thread updated it concurrently
    }

    // Lightweight, zero-blocking notification
    static int s_boostFd = -2;
    if (s_boostFd == -2) {
        const char *runtimeDir = getenv("XDG_RUNTIME_DIR");
        char path[256];
        if (runtimeDir && runtimeDir[0] != '\0') {
            snprintf(path, sizeof(path), "%s/lindows-input-boost", runtimeDir);
        } else {
            snprintf(path, sizeof(path), "/run/user/%u/lindows-input-boost", static_cast<unsigned int>(getuid()));
        }
        s_boostFd = open(path, O_WRONLY | O_CREAT | O_NONBLOCK | O_CLOEXEC, 0644);
    }

    if (s_boostFd >= 0) {
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "%lld\n", static_cast<long long>(nowMs));
        pwrite(s_boostFd, buf, len, 0);
    }
}

} // namespace KWin
