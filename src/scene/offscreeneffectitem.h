/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2026 Xaver Hugl <xaver.hugl@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once
#include "effectwindow.h"
#include "scene/transformitem.h"
#include "utils/filedescriptor.h"

namespace KWin
{

class EglSwapchain;
class EglSwapchainSlot;
class GLShader;
class Texture;
class OffscreenEffect;

class KWIN_EXPORT OffscreenEffectItem : public TransformItem
{
    Q_OBJECT

public:
    explicit OffscreenEffectItem(EffectWindow *window, OffscreenEffect *effect);
    ~OffscreenEffectItem() override;

    void preprocess(ItemRenderer *renderer) override;

    RegionF shape() const override;
    RegionF opaque() const override;

    Texture *texture(RenderDevice *device) const;

    // TODO make this independent of the rendering API and device
    void setShader(GLShader *shader);
    GLShader *shader() const;

    void setHideOriginal(bool hide);

    /**
     * Takes a snapshot of the window and no longer updates it from this moment on
     */
    void freeze();

private:
    WindowQuadList buildQuads(ItemRenderer *renderer) const override;
    void releaseResources(RenderDevice *device) override;
    void handleWindowDamaged();
    void handleWindowGeometryChanged();

    EffectWindow *const m_window;
    OffscreenEffect *const m_effect;
    std::unique_ptr<TransformItem> m_clip;

    std::shared_ptr<EglSwapchain> m_swapchain;
    std::shared_ptr<ColorDescription> m_colorDescription = ColorDescription::sRGB;
    bool m_dirty = true;
    GLShader *m_shader = nullptr;

    bool m_live = true;

    std::unordered_map<RenderDevice *, std::unique_ptr<Texture>> m_textures;
};

}
