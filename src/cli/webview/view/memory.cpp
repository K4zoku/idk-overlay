#include "rhi_texture_extractor.h"
#include "webview.h"

#include <QFile>
#include <QWebEngineSettings>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "core/log.h"

#define PIXELS_SIZE(w, h) ((w) * (h) * 4)

/* Escape a JS source string so it can be embedded as a string literal
 * inside another JS expression. Used by InjectScripts to wrap user
 * scripts in a DOMContentLoaded guard. Escapes backslash, single quote,
 * double quote, newline, carriage return, tab, and other control chars. */
static QString jsToJsString(const QString &s) {
  QString out;
  out.reserve(s.size() + 8);
  out += QChar('"');
  for (const QChar &c : s) {
    ushort u = c.unicode();
    switch (u) {
    case '\\':
      out += QStringLiteral("\\\\");
      break;
    case '"':
      out += QStringLiteral("\\\"");
      break;
    case '\n':
      out += QStringLiteral("\\n");
      break;
    case '\r':
      out += QStringLiteral("\\r");
      break;
    case '\t':
      out += QStringLiteral("\\t");
      break;
    default:
      if (u < 0x20) {
        out += QStringLiteral("\\u%1").arg(u, 4, 16, QChar('0'));
      } else {
        out += c;
      }
      break;
    }
  }
  out += QChar('"');
  return out;
}

WebView::WebView(uint8_t id, const GroupConfig &conf, Manager *manager, bool noDmaBuf, QWidget *parent)
    : QWebEngineView(parent), m_id(id), m_conf(conf), m_manager(manager) {
  m_extractor = new RhiTextureExtractor(this);
  if (noDmaBuf)
    m_useDmaBuf = false;
  setPage(new WebPage);
  settings()->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, false);
  m_renderW = m_conf.width();
  m_renderH = m_conf.height();

  page()->setBackgroundColor(Qt::transparent);
  setAttribute(Qt::WA_TranslucentBackground, true);
  setMinimumSize(m_conf.width(), m_conf.height());
  setMaximumSize(m_conf.width(), m_conf.height());
  load(m_conf.url());

  if (!m_manager->isConnected()) {
    connect(m_manager, &Manager::socketConnected, this, [this]() {
      if (m_memory) {
        IDK_LOG("webview-qt", "Overlay %u reconnected (memory already init'd, skipping re-init)\n", m_id);

        if (auto *fp = focusProxy()) {
          fp->update();
        }

        QTimer::singleShot(100, this, [this]() {
          if (auto *fp = focusProxy())
            fp->update();
        });
        return;
      }
      initDmaBuf();
      initMemory();
      focusProxy()->installEventFilter(this);

      sendCreateImage();
      m_buffer = 0;
      if (auto *fp = focusProxy())
        fp->update();
    });
  } else {
    initDmaBuf();
    initMemory();
    focusProxy()->installEventFilter(this);

    sendCreateImage();
    m_buffer = 0;

    if (auto *fp = focusProxy())
      fp->update();
  }

  m_ackPollTimer = new QTimer(this);
  m_ackPollTimer->setSingleShot(true);
  connect(m_ackPollTimer, &QTimer::timeout, this, [this]() { pollAck(); });

  m_requestTimer = new QTimer(this);
  m_requestTimer->setSingleShot(true);
  connect(m_requestTimer, &QTimer::timeout, this, [this]() { onRequestReceived(); });

  connect(this, &WebView::frameSent, this, [this]() { m_ackPollTimer->start(16); });

  connect(m_manager, &Manager::overlayVisibleChanged, this, &WebView::onOverlayVisibleChanged);

  connect(this, &WebView::loadFinished, this, [this](bool ok) {
    if (!ok || m_conf.url().isEmpty()) {
      IDK_LOG("webview-qt", "loadFinished ok=%d url_empty=%d - skipping script injection\n", (int)ok,
              (int)m_conf.url().isEmpty());
      return;
    }
    QStringList scripts = m_conf.injectScripts();
    IDK_LOG("webview-qt", "loadFinished OK - %lld script(s) to inject\n", scripts.size());
    for (const QString &path : scripts) {
      QFile f(path);
      if (!f.exists()) {
        IDK_LOG("webview-qt", "inject script NOT FOUND: %s\n", path.toUtf8().data());
        continue;
      }
      if (!f.open(QIODevice::ReadOnly)) {
        IDK_LOG("webview-qt", "inject script open FAILED: %s (%s)\n", path.toUtf8().data(),
                f.errorString().toUtf8().data());
        continue;
      }
      QString js = QString::fromUtf8(f.readAll());
      f.close();
      if (js.isEmpty()) {
        IDK_LOG("webview-qt", "inject script EMPTY: %s\n", path.toUtf8().data());
        continue;
      }
      /* Wrap in a DOMContentLoaded guard so the script runs after
       * the page's DOM is ready. The wrapper waits for
       * DOMContentLoaded if needed, else runs immediately. */
      QString wrapped = QStringLiteral("(function(){"
                                       "var js=%1;"
                                       "if(document.readyState==='loading'){"
                                       "document.addEventListener('DOMContentLoaded',function(){try{eval(js);}catch(e){"
                                       "console.error('[idk-overlay] inject error:',e);}});"
                                       "}else{try{eval(js);}catch(e){console.error('[idk-overlay] inject error:',e);}}"
                                       "})()")
                            .arg(jsToJsString(js));
      page()->runJavaScript(wrapped);
      IDK_LOG("webview-qt", "inject script OK: %s (%d bytes)\n", path.toUtf8().data(), (int)js.size());
    }
    if (auto *fp = focusProxy())
      fp->update();
  });
}

void WebView::initMemory() {
  m_memsize = PIXELS_SIZE(m_renderW, m_renderH) * 2;

  m_memfd = memfd_create("idk-webview", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (m_memfd < 0) {
    perror("memfd_create");
    return;
  }

  if (ftruncate(m_memfd, m_memsize) < 0) {
    perror("ftruncate");
    ::close(m_memfd);
    m_memfd = -1;
    return;
  }

  m_memory = mmap(NULL, m_memsize, PROT_READ | PROT_WRITE, MAP_SHARED, m_memfd, 0);
  if (m_memory == MAP_FAILED) {
    perror("mmap");
    ::close(m_memfd);
    m_memfd = -1;
    return;
  }

  fcntl(m_memfd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW);
  fcntl(m_memfd, F_ADD_SEALS, F_SEAL_SEAL);
}
