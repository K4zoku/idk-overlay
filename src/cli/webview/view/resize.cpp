#include "rhi_texture_extractor.h"
#include "webview.h"

#include <QApplication>
#include <QMenu>
#include <QQuickItem>
#include <QQuickWidget>

#include <sys/mman.h>
#include <unistd.h>

#include "core/log.h"

#define PIXELS_SIZE(w, h) ((w) * (h) * 4)

void WebView::resizeForGame(int w, int h) {
  if (w == m_renderW && h == m_renderH)
    return;
  if (m_resizing) {
    IDK_LOG("webview-qt", "game resize: %dx%d (re-entered, skipped)\n", w, h);
    return;
  }
  m_resizing = true;

  IDK_LOG("webview-qt", "game resize: %dx%d -> %dx%d\n", m_renderW, m_renderH, w, h);

  setMinimumSize(w, h);
  setMaximumSize(w, h);
  resize(w, h);

  QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

  /* Toggle the RenderWidgetHostViewQtDelegateItem (QQuickItem) visibility
     to trigger notifyHidden() → notifyShown() on RenderWidgetHostViewQt.
     notifyHidden evicts the old frame and detaches the DelegatedFrameHost;
     notifyShown re-attaches it and requests a new frame from the renderer
     at the current viewport size. */
  if (auto *qw = qobject_cast<QQuickWidget *>(focusProxy())) {
    if (QQuickItem *root = qw->rootObject()) {
      for (auto *child : root->childItems()) {
        child->setVisible(false);
        child->setVisible(true);
      }
    }
  }

  size_t needed = PIXELS_SIZE(w, h) * 2;
  if (m_memsize >= needed) {
    m_renderW = w;
    m_renderH = h;
    m_buffer = 0;
    m_pending = false;
    m_dmaBufFailed = false;
    m_dmabufRejectCount = 0;
    if (auto *fp = focusProxy())
      fp->update();
    m_resizing = false;
    return;
  }

  if (m_memory) {
    munmap(m_memory, m_memsize);
    m_memory = nullptr;
  }
  if (m_memfd >= 0) {
    ::close(m_memfd);
    m_memfd = -1;
  }

  m_renderW = w;
  m_renderH = h;
  m_buffer = 0;

  initMemory();
  m_pending = false;
  m_dmaBufFailed = false;
  m_dmabufRejectCount = 0;

  if (auto *fp = focusProxy())
    fp->update();

  m_resizing = false;
}

void WebView::initDmaBuf() {
  m_eglDpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (m_eglDpy == EGL_NO_DISPLAY) {
    IDK_LOG("webview-qt", "initDmaBuf: eglGetDisplay failed\n");
    m_useDmaBuf = false;
    return;
  }

  EGLint major, minor;
  if (!eglInitialize(m_eglDpy, &major, &minor)) {
    IDK_LOG("webview-qt", "initDmaBuf: eglInitialize failed\n");
    m_eglDpy = EGL_NO_DISPLAY;
    m_useDmaBuf = false;
    return;
  }

  if (!eglBindAPI(EGL_OPENGL_ES_API)) {
    IDK_LOG("webview-qt", "initDmaBuf: eglBindAPI failed\n");
    eglTerminate(m_eglDpy);
    m_eglDpy = EGL_NO_DISPLAY;
    m_useDmaBuf = false;
    return;
  }

  static const EGLint config_attribs[] = {EGL_SURFACE_TYPE,
                                          EGL_PBUFFER_BIT,
                                          EGL_RENDERABLE_TYPE,
                                          EGL_OPENGL_ES2_BIT,
                                          EGL_RED_SIZE,
                                          8,
                                          EGL_GREEN_SIZE,
                                          8,
                                          EGL_BLUE_SIZE,
                                          8,
                                          EGL_ALPHA_SIZE,
                                          8,
                                          EGL_NONE};
  EGLConfig config;
  EGLint ncfg;
  if (!eglChooseConfig(m_eglDpy, config_attribs, &config, 1, &ncfg) || ncfg == 0) {
    IDK_LOG("webview-qt", "initDmaBuf: eglChooseConfig failed\n");
    eglTerminate(m_eglDpy);
    m_eglDpy = EGL_NO_DISPLAY;
    m_useDmaBuf = false;
    return;
  }
  m_eglConfig = config;

  static const EGLint pbuf_attribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
  m_eglSurf = eglCreatePbufferSurface(m_eglDpy, config, pbuf_attribs);
  if (m_eglSurf == EGL_NO_SURFACE) {
    IDK_LOG("webview-qt", "initDmaBuf: eglCreatePbufferSurface failed\n");
    eglTerminate(m_eglDpy);
    m_eglDpy = EGL_NO_DISPLAY;
    m_useDmaBuf = false;
    return;
  }

  static const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  m_eglCtx = eglCreateContext(m_eglDpy, config, EGL_NO_CONTEXT, ctx_attribs);
  if (m_eglCtx == EGL_NO_CONTEXT) {
    IDK_LOG("webview-qt", "initDmaBuf: eglCreateContext failed\n");
    eglDestroySurface(m_eglDpy, m_eglSurf);
    m_eglSurf = EGL_NO_SURFACE;
    eglTerminate(m_eglDpy);
    m_eglDpy = EGL_NO_DISPLAY;
    m_useDmaBuf = false;
    return;
  }
  m_needSharedCtx = true;

  if (m_extractor)
    m_extractor->ensureDmaBufSharedCtx();

  m_queryFn = eglGetProcAddress("eglExportDMABUFImageQueryMESA");
  m_exportFn = eglGetProcAddress("eglExportDMABUFImageMESA");
  if (!m_queryFn || !m_exportFn) {
    m_queryFn = eglGetProcAddress("eglExportDMABUFImageQueryEXT");
    m_exportFn = eglGetProcAddress("eglExportDMABUFImageEXT");
  }
  m_dmabufResolved = (m_queryFn && m_exportFn);

  if (!m_needSharedCtx && m_dmabufResolved)
    IDK_LOG("webview-qt", "Zero-copy DMABUF ready (shared ctx)\n");
  else
    IDK_LOG("webview-qt", "DMABUF deferred (%s%s)\n", m_needSharedCtx ? "no Qt ctx yet" : "",
            !m_dmabufResolved ? "no export funcs" : "");
}

void WebView::contextMenuEvent(QContextMenuEvent *event) {
  QMenu menu;
  menu.addAction(pageAction(QWebEnginePage::Back));
  menu.addAction(pageAction(QWebEnginePage::Forward));
  menu.addAction(pageAction(QWebEnginePage::Reload));
  menu.addSeparator();
  menu.addAction(pageAction(QWebEnginePage::ViewSource));
  menu.addAction(tr("Inspect"), this, [=, this]() {
    if (page()->devToolsPage()) {
      triggerPageAction(QWebEnginePage::InspectElement);
    } else {
      QWebEngineView *view = createWindow(QWebEnginePage::WebDialog);
      view->page()->setInspectedPage(page());
    }
  });
  menu.exec(event->globalPos());
}
