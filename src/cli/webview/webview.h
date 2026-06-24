#pragma once

#include <QWebEngineView>

#define EGL_NO_X11
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "groupconfig.h"
#include "manager.h"
#include "rhi_texture_extractor.h"

/**
 * WebView — renders web content and sends frames to idk-overlay.
 *
 * Adapted from imgoverlay's WebView. Uses QWebEngineView to render web content,
 * then extracts DMA-BUF from Qt6 RHI and sends via idk_fs API.
 */
class WebView : public QWebEngineView
{
    Q_OBJECT

public:
    WebView(uint8_t id, const GroupConfig &conf, Manager *manager, QWidget *parent = nullptr);
    ~WebView();

signals:
    void frameSent();

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;
    QWebEngineView *createWindow(QWebEnginePage::WebWindowType type) override;

private slots:
    void initDmaBuf();
    void sendCreateImage();

private:
    void initShm();
    void initMemory();
    void doRenderAndSend();  // Render webview to SHM + send frame (called via QTimer)

    uint8_t m_id;                       // Overlay ID
    GroupConfig m_conf;                 // Overlay configuration
    Manager *m_manager;                 // Parent manager (socket IPC)

    // SHM state
    int m_memfd = -1;                   // memfd for pixel data
    size_t m_memsize = 0;
    void *m_memory = nullptr;
    uint8_t m_buffer = 0;               // Double-buffer index
    bool m_waitReply = false;

    // ACK flow control
    bool m_pending = false;             // true if frame sent but no ACK yet
    int m_sendTime = 0;                 // timestamp of last send (ms, from QElapsedTimer)

    // EGL state for DMA-BUF export
    bool m_useDmaBuf = true;
    bool m_dmaBufFailed = false;
    EGLDisplay m_eglDpy = EGL_NO_DISPLAY;
    EGLContext m_eglCtx = EGL_NO_CONTEXT;
    EGLSurface m_eglSurf = EGL_NO_SURFACE;

    // Event filter for paint events (SHM mode)
    bool eventFilter(QObject *obj, QEvent *event) override;
};

class WebPage : public QWebEnginePage
{
    Q_OBJECT

public:
    WebPage(QObject *parent = nullptr) : QWebEnginePage(parent) {}

protected:
    void javaScriptConsoleMessage(QWebEnginePage::JavaScriptConsoleMessageLevel level,
                                  const QString &message,
                                  int lineNumber,
                                  const QString &sourceId) override;
};
