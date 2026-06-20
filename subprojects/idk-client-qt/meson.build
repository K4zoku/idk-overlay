project(
    'idk-client-qt',
    'cpp',
    version: '0.1.0',
    default_options: [
        'warning_level=3',
        'cpp_std=c++17',
    ],
    meson_version: '>= 1.0.0',
)

# ── Dependencies ─────────────────────────────────────────────────────────

qt = import('qt6')
qt_dep = dependency('qt6', modules: [
    'Core', 'Gui', 'Widgets', 'WebEngineWidgets', 'QuickWidgets',
], private_headers: true)

egl_dep = dependency('egl')

idk_client_proj = subproject('idk-client')
idk_client_dep = idk_client_proj.get_variable('idk_client_dep')

client_files = files(
    'main.cpp',
    'groupconfig.cpp',
    'manager.cpp',
    'webview.cpp',
    'utils.cpp',
    'rhi_texture_extractor.cpp',
)

client_headers = files(
    'manager.h',
    'webview.h',
    'rhi_texture_extractor.h',
    'groupconfig.h',
    'utils.h',
)

moc_files = qt.preprocess(moc_headers : client_headers,
                          dependencies: qt_dep)

idk_client_qt_exe = executable(
    'idk-client-qt',
    client_files + moc_files,
    dependencies : [ qt_dep, egl_dep, idk_client_dep ],
    install: true,
)

message('  idk-client-qt:      Qt6 webview client (DMA-BUF + idk socket)')
