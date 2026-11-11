/*
    SPDX-FileCopyrightText: 2022 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "scene/itemrenderer_qpainter.h"
#include "core/renderviewport.h"
#include "effect/effect.h"
#include "effect/effecthandler.h"
#include "platformsupport/scenes/qpainter/qpaintersurfacetexture.h"
#include "scene/decorationitem.h"
#include "scene/imageitem.h"
#include "scene/lindows_simd_blit.h"
#include "scene/surfaceitem.h"
#include "scene/windowitem.h"
#include "scene/workspacescene_qpainter.h"
#include "window.h"
#include "workspace.h"

#include <QPainter>
#include <algorithm>
#include <cmath>

namespace KWin
{

ItemRendererQPainter::ItemRendererQPainter()
    : m_painter(std::make_unique<QPainter>())
{
}

ItemRendererQPainter::~ItemRendererQPainter()
{
}

std::unique_ptr<ImageItem> ItemRendererQPainter::createImageItem(Item *parent)
{
    return std::make_unique<ImageItem>(parent);
}

QPainter *ItemRendererQPainter::painter() const
{
    return m_painter.get();
}

void ItemRendererQPainter::beginFrame(const RenderTarget &renderTarget, const RenderViewport &viewport)
{
    QImage *buffer = renderTarget.image();
    m_targetImage = buffer;
    m_painter->begin(buffer);
    m_painter->setWindow(viewport.renderRect().toRect());

    m_backgroundOcclusion = QRegion();
    m_occludedByAbove.clear();

    const bool fullScreenEffect = effects && effects->hasActiveFullScreenEffect();
    if (workspace()) {
        const auto &windows = workspace()->stackingOrder();

        // Prune destroyed/stale items from transformed set
        QSet<const Item *> activeItems;
        for (Window *w : windows) {
            if (w && w->windowItem()) {
                activeItems.insert(w->windowItem());
            }
        }
        m_transformedItems.intersect(activeItems);

        QRegion currentOcclusion;
        // Traverse stacking order from top-most to bottom-most window
        for (auto it = windows.rbegin(); it != windows.rend(); ++it) {
            Window *window = *it;
            if (!window || !window->isShown() || window->isMinimized() || window->isDeleted() || window->isShade()) {
                continue;
            }
            if (window->opacity() < 0.999f) {
                continue;
            }

            WindowItem *windowItem = window->windowItem();
            if (!windowItem || !windowItem->isVisible()) {
                continue;
            }

            const bool windowAnimating = m_transformedItems.contains(windowItem);

            SurfaceItem *surfaceItem = window->surfaceItem();
            QRegion winOpaque;
            if (surfaceItem && surfaceItem->pixmap() && surfaceItem->pixmap()->isValid()) {
                if (window->isDesktop() || !surfaceItem->pixmap()->hasAlphaChannel()) {
                    winOpaque = surfaceItem->mapToScene(QRectF(QPointF(0, 0), surfaceItem->destinationSize())).toAlignedRect();
                } else {
                    winOpaque = surfaceItem->mapToScene(surfaceItem->opaque());
                }
            }

            if (DecorationItem *decorationItem = windowItem->decorationItem()) {
                winOpaque += decorationItem->mapToScene(decorationItem->opaque());
            }

            if (!winOpaque.isEmpty()) {
                if (!window->isDesktop()) {
                    if (!currentOcclusion.isEmpty()) {
                        m_occludedByAbove[windowItem] = currentOcclusion;
                    }
                    if (!windowAnimating && !fullScreenEffect) {
                        currentOcclusion += winOpaque;
                    }
                } else {
                    // Desktop window is at the bottom of the stack; it never occludes other windows,
                    // but it occludes the background unless a full-screen effect is running
                    if (!fullScreenEffect) {
                        currentOcclusion += winOpaque;
                    }
                }
            }
        }
        if (!fullScreenEffect) {
            m_backgroundOcclusion = currentOcclusion;
        }
    }
}

void ItemRendererQPainter::endFrame()
{
    m_painter->end();
    m_targetImage = nullptr;
    m_backgroundOcclusion = QRegion();
    m_occludedByAbove.clear();
}

void ItemRendererQPainter::renderBackground(const RenderTarget &renderTarget, const RenderViewport &viewport, const QRegion &region)
{
    const QRegion culledRegion = region - m_backgroundOcclusion;
    if (culledRegion.isEmpty()) {
        return;
    }

    m_painter->setCompositionMode(QPainter::CompositionMode_Source);
    for (const QRect &rect : culledRegion) {
        m_painter->fillRect(rect, Qt::transparent);
    }
    m_painter->setCompositionMode(QPainter::CompositionMode_SourceOver);
}

void ItemRendererQPainter::renderItem(const RenderTarget &renderTarget, const RenderViewport &viewport, Item *item, int mask, const QRegion &_region, const WindowPaintData &data)
{
    const bool isTransformed = (mask & (Scene::PAINT_WINDOW_TRANSFORMED | Scene::PAINT_SCREEN_TRANSFORMED))
        || (data.opacity() < 0.999f)
        || (data.xScale() != 1.0) || (data.yScale() != 1.0)
        || (data.xTranslation() != 0.0) || (data.yTranslation() != 0.0);

    if (isTransformed) {
        m_transformedItems.insert(item);
    } else {
        m_transformedItems.remove(item);
    }

    QRegion region = _region;

    // Occlusion culling: subtract opaque regions of windows above this item
    if (!m_occludedByAbove.isEmpty()) {
        auto it = m_occludedByAbove.find(item);
        if (it != m_occludedByAbove.end()) {
            region -= it.value();
        }
    }

    const QRect boundingRect = item->mapToScene(item->boundingRect()).toAlignedRect();
    if (!(mask & (Scene::PAINT_WINDOW_TRANSFORMED | Scene::PAINT_SCREEN_TRANSFORMED))) {
        region &= boundingRect;
    }

    if (region.isEmpty()) {
        return;
    }

    m_currentDeviceClip = m_painter->combinedTransform().map(region);

    m_painter->save();
    m_painter->setClipRegion(region);
    m_painter->setClipping(true);
    m_painter->setOpacity(data.opacity());

    if (mask & (Scene::PAINT_WINDOW_TRANSFORMED | Scene::PAINT_SCREEN_TRANSFORMED)) {
        m_painter->translate(item->position());
        m_painter->translate(data.xTranslation(), data.yTranslation());
        m_painter->scale(data.xScale(), data.yScale());
        m_painter->translate(-item->position());
    }

    WindowItem *windowItem = qobject_cast<WindowItem *>(item);
    m_currentWindow = windowItem ? windowItem->window() : nullptr;

    renderItem(m_painter.get(), item);

    m_currentWindow = nullptr;
    m_painter->restore();
}

void ItemRendererQPainter::renderItem(QPainter *painter, Item *item) const
{
    const QList<Item *> sortedChildItems = item->sortedChildItems();

    painter->save();
    painter->translate(item->position());
    painter->setOpacity(painter->opacity() * item->opacity());

    for (Item *childItem : sortedChildItems) {
        if (childItem->z() >= 0) {
            break;
        }
        if (childItem->explicitVisible()) {
            renderItem(painter, childItem);
        }
    }

    item->preprocess();
    if (auto surfaceItem = qobject_cast<SurfaceItem *>(item)) {
        renderSurfaceItem(painter, surfaceItem);
    } else if (auto decorationItem = qobject_cast<DecorationItem *>(item)) {
        renderDecorationItem(painter, decorationItem);
    } else if (auto imageItem = qobject_cast<ImageItem *>(item)) {
        renderImageItem(painter, imageItem);
    }

    for (Item *childItem : sortedChildItems) {
        if (childItem->z() < 0) {
            continue;
        }
        if (childItem->explicitVisible()) {
            renderItem(painter, childItem);
        }
    }

    painter->restore();
}

void ItemRendererQPainter::renderSurfaceItem(QPainter *painter, SurfaceItem *surfaceItem) const
{
    const SurfacePixmap *surfaceTexture = surfaceItem->pixmap();
    if (!surfaceTexture || !surfaceTexture->isValid()) {
        return;
    }

    QPainterSurfaceTexture *platformSurfaceTexture =
        static_cast<QPainterSurfaceTexture *>(surfaceTexture->texture());
    const QImage &srcImage = platformSurfaceTexture->image();
    if (srcImage.isNull()) {
        return;
    }

    const OutputTransform surfaceToBufferTransform = surfaceItem->bufferTransform();
    const QSizeF transformedSize = surfaceToBufferTransform.map(surfaceItem->destinationSize());

    painter->save();
    switch (surfaceToBufferTransform.kind()) {
    case OutputTransform::Normal:
        break;
    case OutputTransform::Rotate90:
        painter->translate(transformedSize.height(), 0);
        painter->rotate(90);
        break;
    case OutputTransform::Rotate180:
        painter->translate(transformedSize.width(), transformedSize.height());
        painter->rotate(180);
        break;
    case OutputTransform::Rotate270:
        painter->translate(0, transformedSize.width());
        painter->rotate(270);
        break;
    case OutputTransform::FlipX:
        painter->translate(transformedSize.width(), 0);
        painter->scale(-1, 1);
        break;
    case OutputTransform::FlipX90:
        painter->scale(-1, 1);
        painter->rotate(90);
        break;
    case OutputTransform::FlipX180:
        painter->translate(0, transformedSize.height());
        painter->scale(-1, 1);
        painter->rotate(180);
        break;
    case OutputTransform::FlipX270:
        painter->translate(transformedSize.height(), transformedSize.width());
        painter->scale(-1, 1);
        painter->rotate(270);
        break;
    }

    const QRectF sourceBox = surfaceItem->bufferSourceBox();
    const qreal xSourceBoxScale = transformedSize.width() > 0 ? sourceBox.width() / transformedSize.width() : 1.0;
    const qreal ySourceBoxScale = transformedSize.height() > 0 ? sourceBox.height() / transformedSize.height() : 1.0;

    // Fast-path: check if transform is unscaled 1:1, unrotated, and aligned to integer pixels
    const bool isIntegerUnscaled = (surfaceToBufferTransform.kind() == OutputTransform::Normal)
        && (painter->worldTransform().type() <= QTransform::TxTranslate)
        && std::abs(xSourceBoxScale - 1.0) < 1e-4
        && std::abs(ySourceBoxScale - 1.0) < 1e-4
        && std::abs(sourceBox.x()) < 1e-4
        && std::abs(sourceBox.y()) < 1e-4;

    const bool isDesktop = m_currentWindow && m_currentWindow->isDesktop();
    const bool isOpaqueSurface = (painter->opacity() >= 0.999f)
        && (isDesktop || !surfaceTexture->hasAlphaChannel() || !srcImage.hasAlphaChannel());

    const QRegion opaqueRegion = isDesktop ? QRegion(QRect(QPoint(0, 0), transformedSize.toSize())) : surfaceItem->opaque();
    const QList<QRectF> shape = surfaceItem->shape();

    for (const QRectF rect : shape) {
        const QRectF target = surfaceToBufferTransform.map(rect, surfaceItem->size());
        const QRectF source(sourceBox.x() + target.x() * xSourceBoxScale,
                            sourceBox.y() + target.y() * ySourceBoxScale,
                            target.width() * xSourceBoxScale,
                            target.height() * ySourceBoxScale);

        // Accelerated SIMD path: fixed-point scale or 1:1 premul blend
        bool handled = false;
        if (surfaceToBufferTransform.kind() == OutputTransform::Normal
            && painter->combinedTransform().type() <= QTransform::TxScale
            && m_targetImage
            && (srcImage.format() == QImage::Format_ARGB32_Premultiplied || srcImage.format() == QImage::Format_RGB32)) {

            const QRectF dstRectF = painter->combinedTransform().mapRect(target);
            const int opacity256 = std::clamp(static_cast<int>(std::round(painter->opacity() * 256.0)), 0, 256);

            handled = lindowsBlendOrScaleBlitSimd(m_targetImage, dstRectF, srcImage, source,
                                                 m_currentDeviceClip, opacity256, isOpaqueSurface);
        }

        if (!handled) {
            if (isIntegerUnscaled) {
                const QRect tRect = target.toRect();
                const QRect sRect = source.toRect();

                if (isOpaqueSurface) {
                    // Completely opaque surface: switch to CompositionMode_Source (direct memcpy!)
                    painter->setCompositionMode(QPainter::CompositionMode_Source);
                    painter->drawImage(tRect, srcImage, sRect);
                    painter->setCompositionMode(QPainter::CompositionMode_SourceOver);
                } else if (painter->opacity() >= 0.999f && !opaqueRegion.isEmpty()) {
                    // Surface has alpha, but has an explicit opaque region:
                    // Fast-path opaque sub-rectangles with Source, remainder with SourceOver
                    const QRegion targetOpaque = opaqueRegion.intersected(tRect);
                    const QRegion targetTranslucent = QRegion(tRect) - targetOpaque;

                    if (!targetOpaque.isEmpty()) {
                        painter->setCompositionMode(QPainter::CompositionMode_Source);
                        for (const QRect &oRect : targetOpaque) {
                            const QRect subSource(sRect.x() + (oRect.x() - tRect.x()),
                                                  sRect.y() + (oRect.y() - tRect.y()),
                                                  oRect.width(), oRect.height());
                            painter->drawImage(oRect, srcImage, subSource);
                        }
                        painter->setCompositionMode(QPainter::CompositionMode_SourceOver);
                    }
                    for (const QRect &trRect : targetTranslucent) {
                        const QRect subSource(sRect.x() + (trRect.x() - tRect.x()),
                                              sRect.y() + (trRect.y() - tRect.y()),
                                              trRect.width(), trRect.height());
                        painter->drawImage(trRect, srcImage, subSource);
                    }
                } else {
                    // Translucent surface: integer drawImage avoids float rasterizer matrix calculations
                    painter->drawImage(tRect, srcImage, sRect);
                }
            } else {
                painter->drawImage(target, srcImage, source);
            }
        }
    }

    painter->restore();
}

void ItemRendererQPainter::renderDecorationItem(QPainter *painter, DecorationItem *decorationItem) const
{
    const auto renderer = static_cast<const SceneQPainterDecorationRenderer *>(decorationItem->renderer());
    if (!renderer) {
        return;
    }

    QRectF dtr, dlr, drr, dbr;
    decorationItem->window()->layoutDecorationRects(dlr, dtr, drr, dbr);

    const bool isScaling = painter->combinedTransform().type() >= QTransform::TxScale;
    const QRectF frameRect = decorationItem->window()->rect();
    const int opacity256 = std::clamp(static_cast<int>(std::round(painter->opacity() * 256.0)), 0, 256);

    auto renderPart = [&](const QRectF &partRect, SceneQPainterDecorationRenderer::DecorationPart part) {
        const QImage &img = renderer->image(part);
        if (img.isNull()) {
            return;
        }

        QRectF targetRect = partRect;
        QRectF sourceRect(0, 0, img.width(), img.height());

        // Motion shadow throttling: when window is scaling (scale/squash/maximize),
        // clip the draw rect to the window frame to skip giant outer drop shadow margins
        if (isScaling) {
            targetRect = partRect.intersected(frameRect);
            if (targetRect.isEmpty()) {
                return;
            }
            const qreal dpr = img.devicePixelRatio();
            const qreal sx = (partRect.width() > 0) ? (img.width() / dpr) / partRect.width() : 1.0;
            const qreal sy = (partRect.height() > 0) ? (img.height() / dpr) / partRect.height() : 1.0;
            sourceRect = QRectF((targetRect.x() - partRect.x()) * sx * dpr,
                                (targetRect.y() - partRect.y()) * sy * dpr,
                                targetRect.width() * sx * dpr,
                                targetRect.height() * sy * dpr);
        }

        bool handled = false;
        if (painter->combinedTransform().type() <= QTransform::TxScale
            && m_targetImage
            && (img.format() == QImage::Format_ARGB32_Premultiplied || img.format() == QImage::Format_RGB32)) {

            const QRectF dstRectF = painter->combinedTransform().mapRect(targetRect);
            handled = lindowsBlendOrScaleBlitSimd(m_targetImage, dstRectF, img, sourceRect,
                                                 m_currentDeviceClip, opacity256, false);
        }

        if (!handled) {
            painter->drawImage(targetRect, img, sourceRect);
        }
    };

    renderPart(dtr, SceneQPainterDecorationRenderer::DecorationPart::Top);
    renderPart(dlr, SceneQPainterDecorationRenderer::DecorationPart::Left);
    renderPart(drr, SceneQPainterDecorationRenderer::DecorationPart::Right);
    renderPart(dbr, SceneQPainterDecorationRenderer::DecorationPart::Bottom);
}

void ItemRendererQPainter::renderImageItem(QPainter *painter, ImageItem *imageItem) const
{
    const QImage &img = imageItem->image();
    if (img.isNull()) {
        return;
    }
    const bool useSource = (painter->opacity() >= 0.999f) && !img.hasAlphaChannel();
    if (useSource) {
        painter->setCompositionMode(QPainter::CompositionMode_Source);
    }
    painter->drawImage(imageItem->rect().toRect(), img);
    if (useSource) {
        painter->setCompositionMode(QPainter::CompositionMode_SourceOver);
    }
}

} // namespace KWin
