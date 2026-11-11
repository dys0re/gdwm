/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2022 Xaver Hugl <xaver.hugl@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "drm_qpainter_layer.h"
#include "core/graphicsbufferview.h"
#include "drm_buffer.h"
#include "drm_gpu.h"
#include "drm_logging.h"
#include "drm_pipeline.h"
#include "drm_virtual_output.h"
#include "platformsupport/scenes/qpainter/qpainterswapchain.h"

#include <cerrno>
#include <drm_fourcc.h>

namespace KWin
{

DrmQPainterLayer::DrmQPainterLayer(DrmPipeline *pipeline, DrmPlane::TypeIndex type)
    : DrmPipelineLayer(pipeline, type)
{
}

uint32_t DrmQPainterLayer::determineFormat() const
{
    if (m_selectedDrmFormat != 0) {
        return m_selectedDrmFormat;
    }
    const auto formats = m_pipeline->formats(m_type);
    if (m_type == DrmPlane::TypeIndex::Primary && formats.contains(DRM_FORMAT_XRGB8888)) {
        return DRM_FORMAT_XRGB8888;
    }
    if (formats.contains(DRM_FORMAT_ARGB8888)) {
        return DRM_FORMAT_ARGB8888;
    }
    return DRM_FORMAT_XRGB8888;
}

bool DrmQPainterLayer::doesSwapchainFit() const
{
    return m_swapchain && m_swapchain->size() == targetRect().size() && (!m_selectedDrmFormat || m_swapchain->format() == m_selectedDrmFormat);
}

std::optional<OutputLayerBeginFrameInfo> DrmQPainterLayer::doBeginFrame()
{
    const QSize size = targetRect().size();
    if (size.isEmpty()) {
        return std::nullopt;
    }

    if (!doesSwapchainFit()) {
        m_shadowValid = false;
        uint32_t format = determineFormat();
        m_swapchain = std::make_shared<QPainterSwapchain>(m_pipeline->gpu()->drmDevice()->allocator(), size, format);
        m_damageJournal = DamageJournal();
        m_currentBuffer = m_swapchain->acquire();

        if (!m_currentBuffer && format == DRM_FORMAT_ARGB8888) {
            format = DRM_FORMAT_XRGB8888;
            m_selectedDrmFormat = DRM_FORMAT_XRGB8888;
            m_swapchain = std::make_shared<QPainterSwapchain>(m_pipeline->gpu()->drmDevice()->allocator(), size, format);
            m_damageJournal = DamageJournal();
            m_currentBuffer = m_swapchain->acquire();
        }

        if (!m_currentBuffer) {
            return std::nullopt;
        }

        m_selectedDrmFormat = format;

        const QImage::Format qFormat = (format == DRM_FORMAT_ARGB8888)
            ? QImage::Format_ARGB32_Premultiplied
            : QImage::Format_RGB32;

        if (!m_shadowBuffer.allocate(size, qFormat)) {
            return std::nullopt;
        }
        m_damageJournal.clear();
    } else {
        m_currentBuffer = m_swapchain->acquire();
        if (!m_currentBuffer) {
            return std::nullopt;
        }
        const QImage::Format qFormat = (m_swapchain->format() == DRM_FORMAT_ARGB8888)
            ? QImage::Format_ARGB32_Premultiplied
            : QImage::Format_RGB32;
        if (m_shadowBuffer.image().isNull() || m_shadowBuffer.image().size() != size || m_shadowBuffer.image().format() != qFormat) {
            if (!m_shadowBuffer.allocate(size, qFormat)) {
                return std::nullopt;
            }
            m_shadowValid = false;
            m_damageJournal.clear();
        }
    }

    m_renderTime = std::make_unique<CpuRenderTimeQuery>();
    const QRegion repaint = m_shadowValid ? QRegion() : infiniteRegion();
    return OutputLayerBeginFrameInfo{
        .renderTarget = RenderTarget(&m_shadowBuffer.image()),
        .repaint = repaint,
    };
}

bool DrmQPainterLayer::doEndFrame(const QRegion &renderedRegion, const QRegion &damagedRegion, OutputFrame *frame)
{
    m_renderTime->end();
    if (frame) {
        frame->addRenderTimeQuery(std::move(m_renderTime));
    }

    const int bufferAge = m_currentBuffer ? m_currentBuffer->age() : 0;
    const QRegion blitRegion = renderedRegion.united(
        m_damageJournal.accumulate(bufferAge, QRegion(QRect(QPoint(0, 0), m_shadowBuffer.image().size())))
    );
    if (m_currentBuffer && m_currentBuffer->view()) {
        lindowsBlitDirtyRegion(m_currentBuffer->view()->image(), m_shadowBuffer.image(), blitRegion);
        m_shadowValid = true;
    }

    m_currentFramebuffer = m_currentBuffer ? m_pipeline->gpu()->importBuffer(m_currentBuffer->buffer(), FileDescriptor{}) : nullptr;
    m_damageJournal.add(damagedRegion);
    if (m_currentBuffer) {
        m_swapchain->release(m_currentBuffer);
    }

    if (!m_currentFramebuffer) {
        qCWarning(KWIN_DRM, "Failed to create dumb framebuffer: %s", strerror(errno));
    }
    return m_currentFramebuffer != nullptr;
}

bool DrmQPainterLayer::checkTestBuffer()
{
    if (!doesSwapchainFit()) {
        const QSize size = targetRect().size();
        uint32_t format = determineFormat();
        m_swapchain = std::make_shared<QPainterSwapchain>(m_pipeline->gpu()->drmDevice()->allocator(), size, format);
        m_currentBuffer = m_swapchain->acquire();
        if (!m_currentBuffer && format == DRM_FORMAT_ARGB8888) {
            format = DRM_FORMAT_XRGB8888;
            m_selectedDrmFormat = DRM_FORMAT_XRGB8888;
            m_swapchain = std::make_shared<QPainterSwapchain>(m_pipeline->gpu()->drmDevice()->allocator(), size, format);
            m_currentBuffer = m_swapchain->acquire();
        }
        if (m_currentBuffer) {
            m_selectedDrmFormat = format;
            m_currentFramebuffer = m_pipeline->gpu()->importBuffer(m_currentBuffer->buffer(), FileDescriptor{});
            m_swapchain->release(m_currentBuffer);
            if (!m_currentFramebuffer) {
                qCWarning(KWIN_DRM, "Failed to create dumb framebuffer: %s", strerror(errno));
            }
        } else {
            m_currentFramebuffer.reset();
        }
    }
    return m_currentFramebuffer != nullptr;
}

std::shared_ptr<DrmFramebuffer> DrmQPainterLayer::currentBuffer() const
{
    return m_currentFramebuffer;
}

void DrmQPainterLayer::releaseBuffers()
{
    m_swapchain.reset();
    m_currentBuffer.reset();
    m_shadowBuffer.free();
    m_shadowValid = false;
}

DrmDevice *DrmQPainterLayer::scanoutDevice() const
{
    return m_pipeline->gpu()->drmDevice();
}

QHash<uint32_t, QList<uint64_t>> DrmQPainterLayer::supportedDrmFormats() const
{
    return m_pipeline->formats(m_type);
}

QList<QSize> DrmQPainterLayer::recommendedSizes() const
{
    return m_pipeline->recommendedSizes(m_type);
}

DrmVirtualQPainterLayer::DrmVirtualQPainterLayer(DrmVirtualOutput *output)
    : DrmOutputLayer(output)
{
}

std::optional<OutputLayerBeginFrameInfo> DrmVirtualQPainterLayer::doBeginFrame()
{
    if (m_image.isNull() || m_image.size() != m_output->modeSize()) {
        m_image = QImage(m_output->modeSize(), QImage::Format_RGB32);
    }
    m_renderTime = std::make_unique<CpuRenderTimeQuery>();
    return OutputLayerBeginFrameInfo{
        .renderTarget = RenderTarget(&m_image),
        .repaint = QRegion(),
    };
}

bool DrmVirtualQPainterLayer::doEndFrame(const QRegion &renderedRegion, const QRegion &damagedRegion, OutputFrame *frame)
{
    m_renderTime->end();
    frame->addRenderTimeQuery(std::move(m_renderTime));
    return true;
}

void DrmVirtualQPainterLayer::releaseBuffers()
{
}

DrmDevice *DrmVirtualQPainterLayer::scanoutDevice() const
{
    // TODO make this use GraphicsBuffers too?
    return nullptr;
}

QHash<uint32_t, QList<uint64_t>> DrmVirtualQPainterLayer::supportedDrmFormats() const
{
    return {{DRM_FORMAT_ARGB8888, QList<uint64_t>{DRM_FORMAT_MOD_LINEAR}}};
}
}
