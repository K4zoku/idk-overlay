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
        'main.cpp',
        'manager.cpp',
        'config/groupconfig.cpp',
        'input/input_receiver.cpp',
        'view/rhi_texture_extractor.cpp',
        'view/webview.cpp',
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
        dependencies: [qt6_dep, egl_dep, gl_dep, idk_fs_dep, vk_dep],
        install: true,
    )
endif
