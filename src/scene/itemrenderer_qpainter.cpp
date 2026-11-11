/*
    SPDX-FileCopyrightText: 2022 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "scene/itemrenderer_qpainter.h"
#include "core/renderviewport.h"
#include "effect/effect.h"
#include "platformsupport/scenes/qpainter/qpaintersurfacetexture.h"
#include "scene/decorationitem.h"
#include "scene/imageitem.h"
#include "scene/surfaceitem.h"
#include "scene/windowitem.h"
#include "scene/workspacescene_qpainter.h"
#include "window.h"
#include "workspace.h"

#include <QPainter>

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
    m_painter->begin(buffer);
    m_painter->setWindow(viewport.renderRect().toRect());

    m_backgroundOcclusion = QRegion();
    m_occludedByAbove.clear();

    if (workspace()) {
        const auto &windows = workspace()->stackingOrder();
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

            SurfaceItem *surfaceItem = window->surfaceItem();
            QRegion winOpaque;
            if (surfaceItem && surfaceItem->pixmap() && surfaceItem->pixmap()->isValid()) {
                if (!surfaceItem->pixmap()->hasAlphaChannel()) {
                    winOpaque = surfaceItem->mapToScene(QRectF(QPointF(0, 0), surfaceItem->destinationSize())).toAlignedRect();
                } else {
                    winOpaque = surfaceItem->mapToScene(surfaceItem->opaque());
                }
            }

            if (DecorationItem *decorationItem = windowItem->decorationItem()) {
                winOpaque += decorationItem->mapToScene(decorationItem->opaque());
            }

            if (!winOpaque.isEmpty()) {
                if (!currentOcclusion.isEmpty()) {
                    m_occludedByAbove[windowItem] = currentOcclusion;
                }
                currentOcclusion += winOpaque;
            }
        }
        m_backgroundOcclusion = currentOcclusion;
    }
}

void ItemRendererQPainter::endFrame()
{
    m_painter->end();
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

    m_painter->save();
    m_painter->setClipRegion(region);
    m_painter->setClipping(true);
    m_painter->setOpacity(data.opacity());

    if (mask & Scene::PAINT_WINDOW_TRANSFORMED) {
        m_painter->translate(data.xTranslation(), data.yTranslation());
        m_painter->scale(data.xScale(), data.yScale());
    }

    renderItem(m_painter.get(), item);

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
        && std::abs(xSourceBoxScale - 1.0) < 1e-4
        && std::abs(ySourceBoxScale - 1.0) < 1e-4
        && std::abs(sourceBox.x()) < 1e-4
        && std::abs(sourceBox.y()) < 1e-4;

    const bool isOpaqueSurface = (painter->opacity() >= 0.999f)
        && (!surfaceTexture->hasAlphaChannel() || !srcImage.hasAlphaChannel());

    const QRegion opaqueRegion = surfaceItem->opaque();
    const QList<QRectF> shape = surfaceItem->shape();

    for (const QRectF rect : shape) {
        const QRectF target = surfaceToBufferTransform.map(rect, surfaceItem->size());
        const QRectF source(sourceBox.x() + target.x() * xSourceBoxScale,
                            sourceBox.y() + target.y() * ySourceBoxScale,
                            target.width() * xSourceBoxScale,
                            target.height() * ySourceBoxScale);

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

    painter->restore();
}

void ItemRendererQPainter::renderDecorationItem(QPainter *painter, DecorationItem *decorationItem) const
{
    const auto renderer = static_cast<const SceneQPainterDecorationRenderer *>(decorationItem->renderer());
    QRectF dtr, dlr, drr, dbr;
    decorationItem->window()->layoutDecorationRects(dlr, dtr, drr, dbr);

    painter->drawImage(dtr.toRect(), renderer->image(SceneQPainterDecorationRenderer::DecorationPart::Top));
    painter->drawImage(dlr.toRect(), renderer->image(SceneQPainterDecorationRenderer::DecorationPart::Left));
    painter->drawImage(drr.toRect(), renderer->image(SceneQPainterDecorationRenderer::DecorationPart::Right));
    painter->drawImage(dbr.toRect(), renderer->image(SceneQPainterDecorationRenderer::DecorationPart::Bottom));
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
