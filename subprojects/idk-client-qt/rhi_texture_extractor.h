#pragma once

#include <QQuickWindow>

/**
 * Extract DMA-BUF texture info from Qt6 RHI.
 * Uses private Qt API to access the render target's underlying DMA-BUF.
 */
class RhiTextureExtractor
{
public:
    struct TextureInfo {
        bool valid = false;
        unsigned int textureId = 0;
        int format = 0;  // EGL/GL format (int, matches eglExportDMABUFImageQueryMESA)
        int nfd = 0;
        unsigned long modifier = 0;
        int dmabufs[4] = {};
        int strides[4] = {};
        int offsets[4] = {};
    };

    /**
     * Extract DMA-BUF texture info from a QtQuick window.
     * 
     * @param window    QtQuick window (QQuickWindow*)
     * @param texInfo   Output structure filled with DMA-BUF info
     * @return          true if extraction succeeded
     */
    static bool extractTextureInfo(QQuickWindow *window, TextureInfo &texInfo);

    /**
     * Clean up EGL resources.
     */
    static void cleanup();

    static void *s_eglImage;  // Defined in rhi_texture_extractor.cpp
};
