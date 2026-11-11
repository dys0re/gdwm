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
    if (formats.contains(DRM_FORMAT_ARGB8888)) {
        return DRM_FORMAT_ARGB8888;
    }
    return DRM_FORMAT_XRGB8888;
}

bool DrmQPainterLayer::doesSwapchainFit() const
{
    return m_swapchain && m_swapchain->size() == targetRect().size() && (!m_selectedDrmFormat || m_swapchain->format() == m_selectedDrmFormat);
}

bool DrmQPainterLayer::ensureSwapchainAndShadow()
{
    const QSize size = targetRect().size();
    if (size.isEmpty()) {
        return false;
    }

    uint32_t format = determineFormat();

    if (!doesSwapchainFit()) {
        m_swapchain = std::make_shared<QPainterSwapchain>(m_pipeline->gpu()->drmDevice()->allocator(), size, format);
        m_damageJournal = DamageJournal();
        m_currentBuffer = m_swapchain->acquire();

        if (!m_currentBuffer && format == DRM_FORMAT_ARGB8888) {
            // Gracefully fall back to DRM_FORMAT_XRGB8888 for XR24-only dumb buffer DRM drivers
            format = DRM_FORMAT_XRGB8888;
            m_selectedDrmFormat = DRM_FORMAT_XRGB8888;
            m_swapchain = std::make_shared<QPainterSwapchain>(m_pipeline->gpu()->drmDevice()->allocator(), size, format);
            m_damageJournal = DamageJournal();
            m_currentBuffer = m_swapchain->acquire();
        }

        if (!m_currentBuffer) {
            return false;
        }

        m_selectedDrmFormat = format;

        const QImage::Format qFormat = (format == DRM_FORMAT_ARGB8888)
            ? QImage::Format_ARGB32_Premultiplied
            : QImage::Format_RGB32;

        if (!m_shadowBuffer.allocate(size, qFormat)) {
            return false;
        }
        m_damageJournal.clear();
    } else if (!m_currentBuffer) {
        m_currentBuffer = m_swapchain->acquire();
        if (!m_currentBuffer) {
            return false;
        }
    }

    return true;
}

std::optional<OutputLayerBeginFrameInfo> DrmQPainterLayer::doBeginFrame()
{
    if (!ensureSwapchainAndShadow()) {
        return std::nullopt;
    }

    m_renderTime = std::make_unique<CpuRenderTimeQuery>();
    const QRegion repaint = m_damageJournal.accumulate(m_currentBuffer->age(), infiniteRegion());
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

    if (m_currentBuffer && m_currentBuffer->view()) {
        lindowsBlitDirtyRegion(m_currentBuffer->view()->image(), m_shadowBuffer.image(), renderedRegion);
    }

    m_currentFramebuffer = m_pipeline->gpu()->importBuffer(m_currentBuffer->buffer(), FileDescriptor{});
    m_damageJournal.add(damagedRegion);
    m_swapchain->release(m_currentBuffer);
    m_currentBuffer.reset();

    if (!m_currentFramebuffer) {
        qCWarning(KWIN_DRM, "Failed to create dumb framebuffer: %s", strerror(errno));
    }
    return m_currentFramebuffer != nullptr;
}

bool DrmQPainterLayer::checkTestBuffer()
{
    if (!doesSwapchainFit()) {
        if (!ensureSwapchainAndShadow()) {
            m_currentFramebuffer.reset();
            return false;
        }
        if (m_currentBuffer) {
            m_currentFramebuffer = m_pipeline->gpu()->importBuffer(m_currentBuffer->buffer(), FileDescriptor{});
            if (!m_currentFramebuffer && m_selectedDrmFormat == DRM_FORMAT_ARGB8888) {
                // If dumb framebuffer import failed for ARGB8888, try XRGB8888 fallback
                m_swapchain.reset();
                m_currentBuffer.reset();
                m_selectedDrmFormat = DRM_FORMAT_XRGB8888;
                if (ensureSwapchainAndShadow() && m_currentBuffer) {
                    m_currentFramebuffer = m_pipeline->gpu()->importBuffer(m_currentBuffer->buffer(), FileDescriptor{});
                }
            }
            if (m_currentBuffer) {
                m_swapchain->release(m_currentBuffer);
                m_currentBuffer.reset();
            }
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
