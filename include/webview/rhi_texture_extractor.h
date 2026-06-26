#pragma once

#include <QQuickWindow>
#include <QQuickWidget>
#include <QSGRendererInterface>

class WebView;

class RhiTextureExtractor
{
public:
    explicit RhiTextureExtractor(WebView *view);
    ~RhiTextureExtractor();

    bool tryExportDMABuf();
    bool tryReadPixelsToSHM(unsigned char *shm, int w, int h);
    bool ensureDmaBufSharedCtx();

private:
    bool tryExportDMABufOpenGL();
    bool tryExportDMABufVulkan();

    static bool fenceSyncGL();
    static void resolveFenceGL();
    static void resolveFBOGL();
    static void *s_fn_glFenceSync;
    static void *s_fn_glClientWaitSync;
    static void *s_fn_glDeleteSync;
    static void *s_fn_glGenFramebuffers;
    static void *s_fn_glBindFramebuffer;
    static void *s_fn_glFramebufferTexture2D;
    static void *s_fn_glBlitFramebuffer;
    static void *s_fn_glReadPixels;
    static void *s_fn_glDeleteFramebuffers;

    WebView *m_view;
};
