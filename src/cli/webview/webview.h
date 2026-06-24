#pragma once

#include <QWebEngineView>
#include <QTimer>

#define EGL_NO_X11
#include <EGL/egl.h>
#include <EGL/eglext.h>

class QSGRendererInterface;
class QQuickWindow;

#ifdef IDK_HAVE_VULKAN
#include <vulkan/vulkan.h>
#endif

#include "groupconfig.h"
#include "manager.h"

/**
 * WebView — renders web content and sends frames to idk-overlay.
 *
 * Two render paths:
 *   1. Zero-copy DMABUF — exports Qt Quick's native GL texture as DMABUF fd
 *      (requires EGL context sharing with Qt + Mesa/EXT export extensions).
 *   2. SHM fallback — grabFramebuffer() → memcpy → send memfd.
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
    void initMemory();
    void doRenderAndSend();
    bool tryExportDMABuf();             // dispatcher — detects backend
    bool tryExportDMABufOpenGL();
    bool tryExportDMABufVulkan();
    bool ensureDmaBufSharedCtx();

    void resizeForGame(int w, int h);
    void initVulkan(QSGRendererInterface *rif, QQuickWindow *window);

    uint8_t m_id;
    GroupConfig m_conf;
    Manager *m_manager;
    int m_renderW = 0;
    int m_renderH = 0;

    // SHM state (fallback path)
    int m_memfd = -1;
    size_t m_memsize = 0;
    void *m_memory = nullptr;
    uint8_t m_buffer = 0;
    bool m_waitReply = false;

    // ACK flow control
    bool m_pending = false;
    int m_sendTime = 0;

    // Resize transition guard — skip frame send after resize until paint event
    bool m_resizePending = false;

    // Re-entrance guard for resizeForGame — prevents nested calls via processEvents
    bool m_resizing = false;

    // SHM format flag — true if grabFramebuffer returned premultiplied alpha
    bool m_framePremultiplied = false;



    // EGL / DMABUF state
    bool m_useDmaBuf = true;
    bool m_dmaBufFailed = false;
    bool m_needSharedCtx = true;
    EGLDisplay m_eglDpy = EGL_NO_DISPLAY;
    EGLConfig  m_eglConfig = nullptr;
    EGLContext m_eglCtx = EGL_NO_CONTEXT;
    EGLSurface m_eglSurf = EGL_NO_SURFACE;
    bool       m_dmabufResolved = false;
    void     (*m_queryFn)(void);       // eglExportDMABUFImageQueryMESA/EXT (MESA-ext signature)
    void     (*m_exportFn)(void);      // eglExportDMABUFImageMESA/EXT

    // Vulkan DMABUF export state
#ifdef IDK_HAVE_VULKAN
    struct {
        VkDevice       device = VK_NULL_HANDLE;
        VkPhysicalDevice physDev = VK_NULL_HANDLE;
        VkInstance     instance = VK_NULL_HANDLE;
        VkQueue        queue = VK_NULL_HANDLE;
        uint32_t       queueFamily = 0;
        VkCommandPool  cmdPool = VK_NULL_HANDLE;
        bool           resolved = false;
    } m_vk;
    PFN_vkGetMemoryFdKHR m_vkGetMemoryFdKHR = nullptr;
#endif

    // Heartbeat timer to poll ACKs when Chromium stops generating paint events
    QTimer *m_heartbeat = nullptr;

    // Event filter for paint events
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
