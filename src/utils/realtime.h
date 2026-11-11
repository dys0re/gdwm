/*
    SPDX-FileCopyrightText: 2022 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "kwin_export.h"

namespace KWin
{

/**
 * Makes the calling thread to use realtime scheduling.
 * Defaults to targetPriority = 50 (SCHED_FIFO), falling back to SCHED_RR or nice -15 if denied.
 */
KWIN_EXPORT void gainRealTime(int targetPriority = 50);

/**
 * Lightweight zero-blocking input boost pulse triggered on user interactions.
 * Coalesced with a ~500ms cooldown timer to prevent syscall spamming.
 */
KWIN_EXPORT void triggerInputBoost();

} // namespace KWin
