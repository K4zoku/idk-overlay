project(
    'idk-webview',
    'cpp',
    version: '0.1.0',
    default_options: [
        'warning_level=3',
        'cpp_std=c++17',
    ],
    meson_version: '>= 1.0.0',
)

qt = import('qt6')
qt_dep = dependency('qt6', modules: [
    'Core', 'Gui', 'Widgets', 'WebEngineWidgets', 'QuickWidgets',
], private_headers: true)

egl_dep = dependency('egl')

idk_fs_proj = subproject('idk-framesource')
idk_fs_dep = idk_fs_proj.get_variable('idk_fs_dep')

webview_files = files(
    'main.cpp',
    'groupconfig.cpp',
    'manager.cpp',
    'webview.cpp',
    'utils.cpp',
    'rhi_texture_extractor.cpp',
)

webview_headers = files(
    'manager.h',
    'webview.h',
    'rhi_texture_extractor.h',
    'groupconfig.h',
    'utils.h',
)

moc_files = qt.preprocess(moc_headers: webview_headers, dependencies: qt_dep)

idk_webview_exe = executable(
    'idk-webview',
    webview_files + moc_files,
    dependencies: [qt_dep, egl_dep, idk_fs_dep],
    install: true,
)

message('  idk-webview: Qt6 webview (DMA-BUF + idk socket)')
