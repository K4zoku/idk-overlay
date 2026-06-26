#include "rhi_texture_extractor.h"

#include <QQuickWindow>
#include <QDebug>

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
#include <QQuickRenderTarget>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wsfinae-incomplete"
#include <QtGui/private/qrhi_p.h>  // Private header for RHI access
#pragma GCC diagnostic pop
#endif

#define EGL_NO_X11
#include <EGL/egl.h>
#include <EGL/eglext.h>

// Static member initialization
void *RhiTextureExtractor::s_eglImage = nullptr;

bool RhiTextureExtractor::extractTextureInfo(QQuickWindow *window, TextureInfo &textureInfo)
{
    textureInfo.valid = false;

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
    if (!window) {
        qCritical() << "RhiTextureExtractor: window is null";
        return false;
    }

    // Access private Qt API to get render target
    // This is the ONLY place in codebase where private headers are used
    QRhiTextureRenderTarget *renderTarget = reinterpret_cast<QRhiTextureRenderTarget*>(
        window->rendererInterface()->getResource(
            window,
            QSGRendererInterface::RhiRedirectRenderTarget
        )
    );

    if (!renderTarget || !renderTarget->description().colorAttachmentAt(0)) {
        qCritical() << "RhiTextureExtractor: No render target or color attachment";
        return false;
    }

    textureInfo.textureId = renderTarget->description()
        .colorAttachmentAt(0)
        ->texture()
        ->nativeTexture()
        .object;

    if (!textureInfo.textureId) {
        qCritical() << "RhiTextureExtractor: Failed to get texture ID";
        return false;
    }

    EGLDisplay dpy = eglGetCurrentDisplay();
    s_eglImage = eglCreateImage(dpy, eglGetCurrentContext(),
                                EGL_GL_TEXTURE_2D,
                                reinterpret_cast<EGLClientBuffer>(textureInfo.textureId),
                                nullptr);

    if (!s_eglImage) {
        qCritical() << "RhiTextureExtractor: Failed to create EGL image";
        return false;
    }

    PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC eglExportDMABUFImageQueryMESA =
        (PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC) eglGetProcAddress("eglExportDMABUFImageQueryMESA");
    PFNEGLEXPORTDMABUFIMAGEMESAPROC eglExportDMABUFImageMESA =
        (PFNEGLEXPORTDMABUFIMAGEMESAPROC) eglGetProcAddress("eglExportDMABUFImageMESA");

    if (!eglExportDMABUFImageQueryMESA || !eglExportDMABUFImageMESA) {
        qCritical() << "RhiTextureExtractor: Failed to resolve EGL DMA-BUF functions";
        eglDestroyImage(dpy, s_eglImage);
        s_eglImage = nullptr;
        return false;
    }

    if (!eglExportDMABUFImageQueryMESA(dpy, s_eglImage,
                                       &textureInfo.format,
                                       &textureInfo.nfd,
                                       &textureInfo.modifier)) {
        qCritical() << "RhiTextureExtractor: Failed to query DMABUF export";
        eglDestroyImage(dpy, s_eglImage);
        s_eglImage = nullptr;
        return false;
    }

    if (!eglExportDMABUFImageMESA(dpy, s_eglImage,
                                  textureInfo.dmabufs,
                                  textureInfo.strides,
                                  textureInfo.offsets)) {
        qCritical() << "RhiTextureExtractor: Failed DMABUF export";
        eglDestroyImage(dpy, s_eglImage);
        s_eglImage = nullptr;
        return false;
    }

    textureInfo.valid = true;
    qDebug() << "RhiTextureExtractor: Successfully extracted texture info"
             << "textureId:" << textureInfo.textureId
             << "format:" << textureInfo.format
             << "nfd:" << textureInfo.nfd;

    return true;
#else
    // Qt5 or older Qt6 without RHI support
    qWarning() << "RhiTextureExtractor: Qt version doesn't support RHI DMA-BUF extraction";
    return false;
#endif
}

void RhiTextureExtractor::cleanup()
{
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
    if (s_eglImage) {
        EGLDisplay dpy = eglGetCurrentDisplay();
        eglDestroyImage(dpy, s_eglImage);
        s_eglImage = nullptr;
        qDebug() << "RhiTextureExtractor: Cleaned up EGL image";
    }
#endif
}
