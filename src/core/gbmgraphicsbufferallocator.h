/*
    SPDX-FileCopyrightText: 2023 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "core/graphicsbufferallocator.h"

struct gbm_device;

namespace KWin
{

class DrmDevice;

class KWIN_EXPORT GbmGraphicsBufferAllocator : public GraphicsBufferAllocator
{
public:
    explicit GbmGraphicsBufferAllocator(DrmDevice *device);
    explicit GbmGraphicsBufferAllocator(gbm_device *device);
    ~GbmGraphicsBufferAllocator() override;

    GraphicsBuffer *allocate(const GraphicsBufferOptions &options) override;

private:
    int drmFd() const;
    gbm_device *gbmDevice() const;

    DrmDevice *m_drmDevice = nullptr;
    gbm_device *m_gbmDevice = nullptr;
};

} // namespace KWin
