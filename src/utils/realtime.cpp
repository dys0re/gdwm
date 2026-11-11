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
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>

namespace KWin
{

namespace
{
void resetChildSchedOnFork()
{
    sched_param sp{};
    sp.sched_priority = 0;
    sched_setscheduler(0, SCHED_OTHER, &sp);
    setpriority(PRIO_PROCESS, 0, 0);
}
} // namespace

void gainRealTime(int targetPriority)
{
    static std::once_flag s_atforkOnce;
    std::call_once(s_atforkOnce, []() {
        pthread_atfork(nullptr, nullptr, resetChildSchedOnFork);
    });

    const long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
    // On single-core systems, use SCHED_RR so multiple RT compositor/input/DRM threads
    // time-slice cleanly without FIFO spin-wait priority inversion; on SMP use SCHED_FIFO.
    const int primaryPolicy = (ncpus <= 1) ? SCHED_RR : SCHED_FIFO;
    const int secondaryPolicy = (ncpus <= 1) ? SCHED_FIFO : SCHED_RR;

    auto tryPolicy = [&](int basePolicy) -> int {
        int maxPrio = sched_get_priority_max(basePolicy);
        int minPrio = sched_get_priority_min(basePolicy);
        if (maxPrio < 0 || minPrio < 0) {
            return -1;
        }
        struct rlimit rlimRt;
        if (getrlimit(RLIMIT_RTPRIO, &rlimRt) == 0 && rlimRt.rlim_cur != RLIM_INFINITY) {
            if (static_cast<rlim_t>(maxPrio) > rlimRt.rlim_cur) {
                maxPrio = static_cast<int>(rlimRt.rlim_cur);
            }
        }
        if (maxPrio < minPrio) {
            return -1;
        }
        sched_param sp{};
        sp.sched_priority = std::clamp(targetPriority, minPrio, maxPrio);
#ifdef SCHED_RESET_ON_FORK
        if (pthread_setschedparam(pthread_self(), basePolicy | SCHED_RESET_ON_FORK, &sp) == 0) {
            return 0;
        }
#endif
        return pthread_setschedparam(pthread_self(), basePolicy, &sp);
    };

    int ret = tryPolicy(primaryPolicy);
    if (ret != 0) {
        ret = tryPolicy(secondaryPolicy);
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
