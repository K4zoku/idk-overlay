# idk-webview — Qt6 webview (DMA-BUF + idk socket)
qt6_dep = dependency(
    'qt6',
    modules: ['Core', 'Gui', 'QuickWidgets', 'WebEngineWidgets', 'Widgets'],
    required: false,
    private_headers: true,
)

if qt6_dep.found()
    qt = import('qt6')
    webview_files = files(
        'groupconfig.cpp',
        'input_receiver.cpp',
        'main.cpp',
        'manager.cpp',
        'rhi_texture_extractor.cpp',
        'utils.cpp',
        'webview.cpp',
    )
    webview_moc_headers = files(
        'input_receiver.h',
        'manager.h',
        'webview.h',
    )
    idk_webview_moc = qt.preprocess(moc_headers: webview_moc_headers, dependencies: qt6_dep)
    idk_webview_exe = executable(
        'idk-webview',
        webview_files + idk_webview_moc,
        dependencies: [qt6_dep, egl_dep, idk_fs_dep],
        install: true,
    )
endif
