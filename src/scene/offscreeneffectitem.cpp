/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2026 Xaver Hugl <xaver.hugl@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "offscreeneffectitem.h"
#include "compositor.h"
#include "core/pixelgrid.h"
#include "core/renderdevice.h"
#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "effect/effect.h"
#include "main.h"
#include "multigpuswapchain.h"
#include "offscreeneffect.h"
#include "opengl/eglcontext.h"
#include "opengl/egldisplay.h"
#include "opengl/eglnativefence.h"
#include "opengl/eglswapchain.h"
#include "opengl/glframebuffer.h"
#include "scene/itemrenderer.h"
#include "scene/texture.h"
#include "scene/windowitem.h"
#include "scene/workspacescene.h"

namespace KWin
{

OffscreenEffectItem::OffscreenEffectItem(EffectWindow *window, OffscreenEffect *effect)
    : TransformItem(window->windowItem()->effectContainer())
    , m_window(window)
    , m_effect(effect)
{
    connect(window, &EffectWindow::windowDamaged, this, &OffscreenEffectItem::handleWindowDamaged);
    connect(window, &EffectWindow::windowOpacityChanged, this, &OffscreenEffectItem::handleWindowDamaged);
    connect(window, &EffectWindow::windowFrameGeometryChanged, this, &OffscreenEffectItem::handleWindowGeometryChanged);
}

OffscreenEffectItem::~OffscreenEffectItem()
{
}

void OffscreenEffectItem::preprocess(ItemRenderer *renderer)
{
    const qreal scale = m_window->screen()->scale();
    const RectF logicalGeometry = snapToPixels(m_window->expandedGeometry(), scale);
    const QSize textureSize = (logicalGeometry.size() * scale).toSize();

    if (textureSize.isEmpty()) {
        m_swapchain.reset();
        return;
    }
    // Capture the window in the color space the scene is blended in, so that a window with a wider
    // color description than sRGB keeps its colors while it is redirected.
    const std::shared_ptr<ColorDescription> &colorDescription = m_window->screen()->blendingColor();

    const auto device = Compositor::self()->primaryDevice();
    const FormatModifierMap &supportedFormats = device->eglDisplay()->nonExternalOnlySupportedDrmFormats();
    // An 8 bit intermediate bands, most visibly across an HDR blending space's luminance range, so
    // ask for 16 and let chooseFormat settle on what this device actually supports.
    const auto chosen = MultiGpuSwapchain::chooseFormat(DRM_FORMAT_ARGB16161616, supportedFormats, supportedFormats)
                            .value_or(DrmFormat{
                                .format = DRM_FORMAT_ARGB8888,
                                .modifiers = supportedFormats[DRM_FORMAT_ARGB8888],
                            });

    if (!m_swapchain || m_swapchain->size() != textureSize || m_swapchain->format() != chosen.format) {
        GraphicsBufferOptions options{
            .size = textureSize,
            .format = chosen.format,
            .modifiers = chosen.modifiers,
            .software = device->isSoftwareDevice(),
            .scanout = false,
        };
        m_swapchain = EglSwapchain::create(device, options);
        if (!m_swapchain) {
            return;
        }
        m_dirty = true;
    }
    if (m_colorDescription != colorDescription) {
        m_colorDescription = colorDescription;
        m_dirty = true;
    }

    if (!m_dirty) {
        return;
    }
    auto slot = m_swapchain->acquire();
    if (!slot) {
        return;
    }
    RenderTarget renderTarget(slot->framebuffer(), m_colorDescription);
    RenderViewport viewport(logicalGeometry, scale, renderTarget, QPoint());
    GLFramebuffer::pushFramebuffer(slot->framebuffer());

    const Region repaint = Rect(QPoint(), slot->framebuffer()->size());
    renderer->renderBackground(renderTarget, viewport, repaint);
    if (!renderer->renderItem(renderTarget, viewport, m_window->windowItem()->effectContainer(), 0, repaint, WindowPaintData{}, {}, {})) {
        return;
    }

    GLFramebuffer::popFramebuffer();
    EGLNativeFence fence(m_swapchain->context()->displayObject());
    m_swapchain->release(slot, fence.fileDescriptor().duplicate());

    auto &texture = m_textures[renderer->renderDevice()];
    if (texture) {
        texture->attach(slot->buffer(), fence.takeFileDescriptor(), repaint, slot->releasePoint(), m_colorDescription);
    } else {
        texture = renderer->createTexture(slot->buffer(), fence.takeFileDescriptor(), slot->releasePoint(), m_colorDescription);
    }

    m_dirty = false;
    return;
}

RegionF OffscreenEffectItem::shape() const
{
    return rect();
}

RegionF OffscreenEffectItem::opaque() const
{
    return RegionF{};
}

void OffscreenEffectItem::handleWindowDamaged()
{
    m_dirty = true;
}

void OffscreenEffectItem::handleWindowGeometryChanged()
{
    setGeometry(m_window->rect());
}

Texture *OffscreenEffectItem::texture(RenderDevice *device) const
{
    const auto it = m_textures.find(device);
    return it == m_textures.end() ? nullptr : it->second.get();
}

void OffscreenEffectItem::releaseResources(RenderDevice *device)
{
    m_textures.erase(device);
}

void OffscreenEffectItem::setShader(GLShader *shader)
{
    m_shader = shader;
}

GLShader *OffscreenEffectItem::shader() const
{
    return m_shader;
}

WindowQuadList OffscreenEffectItem::buildQuads(ItemRenderer *renderer) const
{
    const RectF geoRect = rect();
    if (geoRect.isEmpty()) {
        return WindowQuadList{};
    }

    WindowQuad quad;
    quad[0] = WindowVertex(geoRect.topLeft(), geoRect.topLeft());
    quad[1] = WindowVertex(geoRect.topRight(), geoRect.topRight());
    quad[2] = WindowVertex(geoRect.bottomRight(), geoRect.bottomRight());
    quad[3] = WindowVertex(geoRect.bottomLeft(), geoRect.bottomLeft());

    WindowQuadList ret;
    ret.append(quad);
    if (m_effect) {
        WindowPaintData data;
        m_effect->apply(m_window, 0, data, ret);
    }
    return ret;
}

void OffscreenEffectItem::setHideOriginal(bool hide)
{
    if (hide == bool(m_clip)) {
        return;
    }
    if (hide) {
        m_clip = std::make_unique<TransformItem>(m_window->windowItem()->effectContainer());
        m_clip->setGlobalClipRect(RectF{});
    } else {
        m_clip.reset();
    }
}

void OffscreenEffectItem::freeze()
{
    const auto device = Compositor::self()->primaryDevice();
    // TODO let the renderer handle the context
    const auto context = device->eglContext();
    if (!context || !context->makeCurrent()) {
        return;
    }
    preprocess(kwinApp()->scene()->renderer(device));
}

}

#include "moc_offscreeneffectitem.cpp"
