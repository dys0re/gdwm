/*
    SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "effect/offscreeneffect.h"
#include "effecthandler.h"
#include "scene/offscreeneffectitem.h"
#include "scene/surfaceitem.h"

namespace KWin
{

struct OffscreenData
{
public:
    std::unique_ptr<OffscreenEffectItem> m_item;
};

class OffscreenEffectPrivate
{
public:
    std::map<EffectWindow *, std::unique_ptr<OffscreenData>> windows;
    QMetaObject::Connection windowDeletedConnection;
    RenderGeometry::VertexSnappingMode vertexSnappingMode = RenderGeometry::VertexSnappingMode::Round;
};

OffscreenEffect::OffscreenEffect(QObject *parent)
    : Effect(parent)
    , d(std::make_unique<OffscreenEffectPrivate>())
{
}

OffscreenEffect::~OffscreenEffect() = default;

bool OffscreenEffect::supported()
{
    return effects->isOpenGLCompositing();
}

void OffscreenEffect::redirect(EffectWindow *window)
{
    std::unique_ptr<OffscreenData> &offscreenData = d->windows[window];
    if (offscreenData) {
        return;
    }
    offscreenData = std::make_unique<OffscreenData>();
    offscreenData->m_item = std::make_unique<OffscreenEffectItem>(window, this);
    offscreenData->m_item->setVertexSnappingMode(d->vertexSnappingMode);
    offscreenData->m_item->setHideOriginal(true);

    if (d->windows.size() == 1) {
        setupConnections();
    }
}

void OffscreenEffect::unredirect(EffectWindow *window)
{
    auto it = d->windows.find(window);
    if (it == d->windows.end()) {
        return;
    }

    d->windows.erase(it);
}

void OffscreenEffect::setShader(EffectWindow *window, GLShader *shader)
{
    if (const auto it = d->windows.find(window); it != d->windows.end()) {
        it->second->m_item->setShader(shader);
    }
}

void OffscreenEffect::apply(EffectWindow *window, int mask, WindowPaintData &data, WindowQuadList &quads)
{
}

void OffscreenEffect::handleWindowDeleted(EffectWindow *window)
{
    unredirect(window);
}

void OffscreenEffect::setupConnections()
{
    d->windowDeletedConnection =
        connect(effects, &EffectsHandler::windowDeleted, this, &OffscreenEffect::handleWindowDeleted);
}

void OffscreenEffect::destroyConnections()
{
    disconnect(d->windowDeletedConnection);

    d->windowDeletedConnection = {};
}

void OffscreenEffect::setVertexSnappingMode(RenderGeometry::VertexSnappingMode mode)
{
    d->vertexSnappingMode = mode;
    for (auto &window : std::as_const(d->windows)) {
        window.second->m_item->setVertexSnappingMode(mode);
    }
}

bool OffscreenEffect::blocksDirectScanout() const
{
    return false;
}

class CrossFadeWindowData : public OffscreenData
{
public:
    RectF frameGeometryAtCapture;
    RectF expandedGeometryAtCapture;
    double m_progress = 0;
};

class CrossFadeEffectPrivate
{
public:
    std::map<EffectWindow *, std::unique_ptr<CrossFadeWindowData>> windows;
    qreal progress;
};

CrossFadeEffect::CrossFadeEffect(QObject *parent)
    : Effect(parent)
    , d(std::make_unique<CrossFadeEffectPrivate>())
{
}

CrossFadeEffect::~CrossFadeEffect() = default;

static void updateWindow(EffectWindow *window, CrossFadeWindowData *data)
{
    const RectF frameGeometry = window->frameGeometry();
    const RectF &capturedFrameGeometry = data->frameGeometryAtCapture;
    const RectF &capturedExpandedGeometry = data->expandedGeometryAtCapture;

    // This is for the case of *non* live effect, when the window buffer we saved has a different size
    // compared to the size the window has now. The "old" window will be rendered scaled to the current
    // window geometry, but everything will be scaled, also the shadow if there is any, making the window
    // frame not line up anymore with window->frameGeometry()
    // to fix that, we consider how much the shadow will have scaled, and use that as margins to the
    // current frame geometry. this causes the scaled window to visually line up perfectly with frameGeometry,
    // having the scaled shadow all outside of it.
    const qreal widthRatio = capturedFrameGeometry.width() / frameGeometry.width();
    const qreal heightRatio = capturedFrameGeometry.height() / frameGeometry.height();

    const QMarginsF margins{
        (capturedExpandedGeometry.x() - capturedFrameGeometry.x()) / widthRatio,
        (capturedExpandedGeometry.y() - capturedFrameGeometry.y()) / heightRatio,
        (capturedFrameGeometry.right() - capturedExpandedGeometry.right()) / widthRatio,
        (capturedFrameGeometry.bottom() - capturedExpandedGeometry.bottom()) / heightRatio,
    };

    const RectF geometry = RectF(QPointF(0, 0), frameGeometry.size()) - margins;
    data->m_item->setGeometry(geometry);
}

void CrossFadeEffect::prePaintScreen(ScreenPrePaintData &data)
{
    for (const auto &[window, data] : d->windows) {
        updateWindow(window, data.get());
    }
}

void CrossFadeEffect::redirect(EffectWindow *window)
{
    if (d->windows.empty()) {
        connect(effects, &EffectsHandler::windowDeleted, this, &CrossFadeEffect::handleWindowDeleted);
    }

    std::unique_ptr<CrossFadeWindowData> &offscreenData = d->windows[window];
    if (offscreenData) {
        return;
    }
    offscreenData = std::make_unique<CrossFadeWindowData>();
    offscreenData->m_item = std::make_unique<OffscreenEffectItem>(window, nullptr);
    offscreenData->m_item->setZ(100);
    offscreenData->m_item->freeze();

    // Avoid including blur and contrast effects. During a normal painting cycle they
    // won't be included, but since we call effects->drawWindow() outside usual compositing
    // cycle, we have to prevent backdrop effects kicking in.
    const QVariant blurRole = window->data(WindowForceBlurRole);
    window->setData(WindowForceBlurRole, QVariant());

    offscreenData->frameGeometryAtCapture = window->frameGeometry();
    offscreenData->expandedGeometryAtCapture = window->expandedGeometry();
    updateWindow(window, offscreenData.get());

    window->setData(WindowForceBlurRole, blurRole);
}

void CrossFadeEffect::unredirect(EffectWindow *window)
{
    auto it = d->windows.find(window);
    if (it == d->windows.end()) {
        return;
    }

    d->windows.erase(it);
    if (d->windows.empty()) {
        disconnect(effects, &EffectsHandler::windowDeleted, this, &CrossFadeEffect::handleWindowDeleted);
    }
}

void CrossFadeEffect::handleWindowDeleted(EffectWindow *window)
{
    unredirect(window);
}

void CrossFadeEffect::setAllCrossfadeProgress(double value)
{
    for (const auto &[window, data] : d->windows) {
        data->m_progress = value;
        data->m_item->setOpacity(1 - value);
    }
}

void CrossFadeEffect::setCrossfadeProgress(EffectWindow *window, double value)
{
    const auto it = d->windows.find(window);
    if (it == d->windows.end()) {
        return;
    }
    const auto &data = it->second;
    data->m_progress = value;
    data->m_item->setOpacity(1 - value);
}

void CrossFadeEffect::setShader(EffectWindow *window, GLShader *shader)
{
    if (const auto it = d->windows.find(window); it != d->windows.end()) {
        it->second->m_item->setShader(shader);
    }
}

bool CrossFadeEffect::blocksDirectScanout() const
{
    return false;
}

} // namespace KWin

#include "moc_offscreeneffect.cpp"
