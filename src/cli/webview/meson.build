# idk-webview — Qt6 webview (DMA-BUF + idk socket)
qt6_dep = dependency(
    'qt6',
    modules: ['Core', 'Gui', 'QuickWidgets', 'WebEngineWidgets', 'Widgets'],
    required: false,
    private_headers: true,
)

if qt6_dep.found()
    webview_inc = include_directories('../../../include/webview')

    qt = import('qt6')
    webview_files = files(
        'config/groupconfig.cpp',
        'input/events.cpp',
        'input/keys.cpp',
        'input/repeat.cpp',
        'input/socket.cpp',
        'main.cpp',
        'manager/config.cpp',
        'manager/connect.cpp',
        'manager/js.cpp',
        'manager/views.cpp',
        'view/capture.cpp',
        'view/memory.cpp',
        'view/poll.cpp',
        'view/resize.cpp',
        'view/rhi/common.cpp',
        'view/rhi/gbm.cpp',
        'view/rhi/gl_dmabuf.cpp',
        'view/rhi/readpixels.cpp',
        'view/rhi/vk_dmabuf.cpp',
    )
    webview_moc_headers = files(
        '../../../include/webview/input_receiver.h',
        '../../../include/webview/manager.h',
        '../../../include/webview/webview.h',
    )
    idk_webview_moc = qt.preprocess(moc_headers: webview_moc_headers, dependencies: qt6_dep)
    idk_webview_exe = executable(
        'idk-webview',
        webview_files + idk_webview_moc,
        include_directories: [inc, webview_inc],
        dependencies: [qt6_dep, egl_dep, gl_dep, idk_producer_dep, vk_dep, gbm_dep],
        install: true,
    )
endif
