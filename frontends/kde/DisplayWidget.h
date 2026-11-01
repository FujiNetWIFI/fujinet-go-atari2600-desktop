/*
 * The emulator display on the GPU: a QRhiWidget that keeps one texture the
 * size of the frame and draws one textured quad, only when the session
 * has published a new frame (see FrameFeed). Qt picks Vulkan, Metal, D3D or
 * OpenGL for it; the scaling, aspect and filtering all happen on the GPU.
 *
 * If no GPU device can be had it emits renderFailed() and the main window
 * swaps in the software SoftDisplayWidget.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QImage>
#include <QRhiWidget>
#include <memory>
#include <rhi/qrhi.h>

#include "DisplayView.h"
#include "FrameFeed.h"
#include "a2600session.h"

class DisplayWidget : public QRhiWidget, public DisplayView {
    Q_OBJECT
public:
    explicit DisplayWidget(a2600session *session, QWidget *parent = nullptr);
    ~DisplayWidget() override;

    QWidget *widget() override { return this; }
    void setTvAspect(bool tv) override;
    void setSmooth(bool smooth) override;

protected:
    void initialize(QRhiCommandBuffer *cb) override;
    void render(QRhiCommandBuffer *cb) override;
    void releaseResources() override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    bool event(QEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *e) override;
    /* Tab is a key like any other here (it can be bound to a control): it
     * reaches the window's key handler instead of moving the focus. */
    bool focusNextPrevChild(bool) override { return false; }

private:
    void onFrame();
    void updateActive();
    void rebuildTexture(int lines);

    a2600session *m_session;
    FrameFeed *m_feed;
    QImage m_scratch;          /* FB_WIDTH x FB_MAX_HEIGHT, filled by copy_frame */
    uint64_t m_serial = 0;
    int m_lines = 0;           /* lines in the newest frame */
    bool m_dirty = false;      /* m_scratch holds a frame not yet uploaded */
    bool m_tvAspect = true;
    bool m_smooth = false;
    bool m_smoothApplied = false;

    QRhi *m_rhi = nullptr;
    std::unique_ptr<QRhiBuffer> m_ubuf;
    std::unique_ptr<QRhiTexture> m_tex;
    std::unique_ptr<QRhiSampler> m_nearest, m_linear;
    std::unique_ptr<QRhiShaderResourceBindings> m_srb;
    std::unique_ptr<QRhiGraphicsPipeline> m_pipeline;
    int m_texLines = 0;
};
