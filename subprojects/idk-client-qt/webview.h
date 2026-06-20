#pragma once

#include <QWebEngineView>
#include <QTimer>

#include "groupconfig.h"
#include "manager.h"
#include "rhi_texture_extractor.h"

/**
 * WebView — renders web content and sends frames to idk-overlay.
 *
 * Adapted from imgoverlay's WebView. Uses QWebEngineView to render web content,
 * then extracts DMA-BUF from Qt6 RHI and sends via idk_client API.
 */
class WebView : public QWebEngineView
{
    Q_OBJECT

public:
    /**
     * Create a new WebView for an overlay group.
     * 
     * @param id      Overlay ID (1-based)
     * @param conf    Configuration from INI file
     * @param manager Parent manager (for socket IPC)
     * @param parent  Parent widget
     */
    WebView(uint8_t id, const GroupConfig &conf, Manager *manager, QWidget *parent = nullptr);
    ~WebView();

signals:
    /**
     * Emitted when a frame is successfully sent.
     */
    void frameSent();

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;
    QWebEngineView *createWindow(QWebEnginePage::WebWindowType type) override;

private slots:
    /**
     * Initialize DMA-BUF extraction when QtQuick window is ready (Qt6 only).
     */
    void initDmaBuf();

    /**
     * Send create image message to idk-overlay via manager.
     */
    void sendCreateImage();

private:
    /**
     * Initialize SHM-based frame sending (fallback for Qt5 or no DMA-BUF).
     */
    void initShm();

    /**
     * Allocate shared memory for pixel data.
     */
    void initMemory();

    uint8_t m_id;                       // Overlay ID
    GroupConfig m_conf;                 // Overlay configuration
    Manager *m_manager;                 // Parent manager (socket IPC)

    // SHM state
    int m_memfd = -1;                   // memfd for pixel data
    size_t m_memsize = 0;
    void *m_memory = nullptr;
    uint8_t m_buffer = 0;               // Double-buffer index
    bool m_waitReply = false;
    QTimer *m_updateTimer = nullptr;

    // DMA-BUF state (Qt6 RHI)
    bool m_useDmaBuf = true;
    unsigned int m_fbo = 0;
    quint64 m_format = 0;
    int m_nfd = 0;
    unsigned long m_modifier = 0;
    int m_strides[4] = {};
    int m_offsets[4] = {};
    int m_dmabufs[4] = {};

    // Event filter for paint events (SHM mode)
    bool eventFilter(QObject *obj, QEvent *event) override;
};

/**
 * Custom WebPage for console message handling.
 */
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
