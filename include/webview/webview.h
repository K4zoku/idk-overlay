#pragma once
#include <QOpenGLFunctions>
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

class RhiTextureExtractor;

class WebView : public QWebEngineView
{
    Q_OBJECT
    friend class RhiTextureExtractor;

public:
    WebView(uint8_t id, const GroupConfig &conf, Manager *manager, bool noDmaBuf = false, QWidget *parent = nullptr);
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

    void resizeForGame(int w, int h);

public:
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
    /* Consecutive DMABUF rejection counter. Incremented on each ack=1 from
     * compositor, reset on ack=0. Only set m_dmaBufFailed=true after 5
     * consecutive rejections — handles transient failures (first frame
     * after resize, NVIDIA driver hiccups on vkGetMemoryFdPropertiesKHR). */
    int m_dmabufRejectCount = 0;
    bool m_needSharedCtx = true;
    /* Cooldown timestamp (monotonic ms) after a resize — Qt RHI is
     * rebuilding its render-target texture during this window and
     * exporting DMABUF mid-rebuild can SIGSEGV. Force SHM until it expires. */
    qint64 m_dmabufCooldownUntil = 0;
    EGLDisplay m_eglDpy = EGL_NO_DISPLAY;
    EGLConfig  m_eglConfig = nullptr;
    EGLContext m_eglCtx = EGL_NO_CONTEXT;
    EGLSurface m_eglSurf = EGL_NO_SURFACE;
    bool       m_dmabufResolved = false;
    void     (*m_queryFn)(void);       // eglExportDMABUFImageQueryMESA/EXT (MESA-ext signature)
    void     (*m_exportFn)(void);      // eglExportDMABUFImageMESA/EXT
    /* Our own GL texture for hybrid DMABUF path — grabFramebuffer() →
     * glTexSubImage2D → export as dmabuf. Bypasses Qt RHI's texture
     * which has stale/white content when exported directly. */
    GLuint m_dmaTex = 0;
    int m_dmaTexW = 0;
    int m_dmaTexH = 0;
    /* Cached EGLImage + exported dmabuf fd. Reused across frames as long
     * as m_dmaTex / size don't change — avoids recreating eglCreateImage +
     * eglExportDMABUFImageMESA every frame (expensive on Mesa). The fd is
     * dup'd per send (sendmsg takes ownership of the dup, not the original).
     * On size change or shutdown, both are destroyed. */
    EGLImageKHR m_dmaEglImg = EGL_NO_IMAGE_KHR;
    int m_dmaExportFd = -1;
    uint32_t m_dmaExportFourcc = 0;
    uint32_t m_dmaExportStride = 0;
    uint64_t m_dmaExportModifier = 0;

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

    RhiTextureExtractor *m_extractor = nullptr;

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
