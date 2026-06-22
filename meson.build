project(
    'idk-overlay',
    ['c', 'cpp'],
    version: '0.1.0',
    default_options: [
        'warning_level=3',
        'c_std=gnu11',
    ],
    meson_version: '>= 1.0.0',
)

add_project_arguments('-fPIC', language: 'c')
add_project_arguments('-fPIC', language: 'cpp')

cc = meson.get_compiler('c')
pthread_lib = cc.find_library('pthread', required: true)
dl_lib = cc.find_library('dl', required: true)

syringe_proj = subproject('syringe')
syringe_dep = syringe_proj.get_variable('libsyringe_dep')

overlay_c_flags = [
    '-D_GNU_SOURCE',
    '-Wall',
    '-Wextra',
    '-O2',
    '-Wno-pedantic',
    '-fno-stack-protector',
]
fs_c_flags = ['-D_GNU_SOURCE', '-Wall', '-Wextra', '-O2']

# ── Include directories ────────────────────────────────────────────────────
inc = include_directories('include')
inc_public = include_directories('include/public')

# ── Shader embedding ───────────────────────────────────────────────────────
python3 = find_program('python3', required: false)
shader_srcs = files(
    'shaders/overlay_120.frag',
    'shaders/overlay_120.vert',
    'shaders/overlay_130.frag',
    'shaders/overlay_130.vert',
    'shaders/overlay_300_es.frag',
    'shaders/overlay_300_es.vert',
    'shaders/overlay_410.frag',
    'shaders/overlay_410.vert',
)
if python3.found()
    overlay_shader_embed_h = custom_target(
        'embed-shaders',
        input: shader_srcs,
        output: 'overlay_shader_embed.h',
        command: [
            python3,
            meson.current_source_dir() / 'scripts/embed_shaders.py',
            '--output', '@OUTPUT@',
            '--shader-dir', meson.current_source_dir() / 'shaders',
        ],
        build_by_default: true,
    )
else
    overlay_shader_embed_h = files('src/overlay_shader_embed.h')
endif

vk_found = cc.has_function('vkCreateInstance', prefix: '#include <vulkan/vulkan.h>')

# ── libidk-shim.so — dlsym interceptor (LD_PRELOAD first) ─────────────────
idk_shim_so = library(
    'idk-shim',
    ['src/shim/idk_shim.c', 'src/shim/elfhacks.c'],
    c_args: overlay_c_flags,
    include_directories: [inc, inc_public],
    link_args: ['-ldl', '-lpthread'],
    version: '0.1',
    install: true,
)

# ── libidk-gl.so — GL/EGL hooks + compositor (LD_PRELOAD path) ─────────────
egl_dep = dependency('egl', required: false)
gles2_dep = dependency('glesv2', required: false)
if egl_dep.found() and gles2_dep.found()
    idk_gl_so = library(
        'idk-gl',
        [
            'src/shim/idk_gl_hook.c',
            'src/core/compositor.c',
            'src/gl/gl_loader.c',
            overlay_shader_embed_h,
        ],
        c_args: overlay_c_flags,
        include_directories: [inc, inc_public],
        dependencies: [egl_dep, gles2_dep],
        link_args: ['-ldl', '-lpthread'],
        version: '0.1',
        install: true,
    )
else
    warning('EGL/GLES2 dev headers not found — libidk-gl.so will not be built.')
endif

# ── libidk-overlay.so — inject path (EGL + Vulkan hooks) ───────────────────
egl_dep2 = dependency('egl', required: false)
idk_overlay_so = library(
    'idk-overlay',
    [
        'src/inject/overlay.c',
        'src/gl/egl_hook.c',
        'src/core/compositor.c',
        'src/gl/gl_loader.c',
        'src/inject/vulkan_hook.c',
        'src/ipc/ipc.c',
        overlay_shader_embed_h,
    ],
    c_args: overlay_c_flags,
    include_directories: [inc, inc_public],
    dependencies: [syringe_dep, egl_dep2],
    link_args: ['-ldl', '-lpthread'],
    version: '0.1',
    install: true,
)

# ── idk-render ─────────────────────────────────────────────────────────────
idk_render_exe = executable(
    'idk-render',
    ['src/render/render.c', 'src/render/receive.c', 'src/ipc/ipc.c'],
    c_args: overlay_c_flags,
    include_directories: [inc, inc_public],
    link_args: ['-ldl', '-lpthread'],
    dependencies: [syringe_dep, pthread_lib],
    install: true,
)

# ── idk-inject ─────────────────────────────────────────────────────────────
idk_inject_exe = executable(
    'idk-inject',
    'src/inject/inject.c',
    c_args: overlay_c_flags,
    include_directories: [inc, inc_public],
    link_args: ['-ldl', '-lpthread'],
    dependencies: [syringe_dep],
    install: true,
)

# ── libidk-framesource.so — frame sender library (built inline) ────────────
idk_fs_lib = library(
    'idk-framesource',
    'src/idk_fs.c',
    c_args: fs_c_flags,
    include_directories: [inc, inc_public],
    link_args: ['-ldl', '-lpthread'],
    version: '0.1',
    install: true,
)

idk_fs_dep = declare_dependency(
    include_directories: [inc, inc_public],
    link_with: idk_fs_lib,
)

# ── idk-send-frame (optional CLI — links libidk-framesource) ───────────────
idk_send_frame_exe = executable(
    'idk-send-frame',
    'src/idk_send_frame.c',
    c_args: overlay_c_flags,
    include_directories: [inc, inc_public],
    link_args: ['-ldl', '-lpthread'],
    dependencies: [idk_fs_dep, pthread_lib],
    install: true,
)

# ── idk-webview (optional — requires Qt6WebEngine) ─────────────────────────
if not meson.is_subproject()
    qt6_dep = dependency(
        'qt6',
        modules: ['Core', 'Gui', 'QuickWidgets', 'WebEngineWidgets', 'Widgets'],
        required: false,
        private_headers: true,
    )
    if qt6_dep.found()
        qt = import('qt6')
        webview_files = files(
            'tools/idk-webview/groupconfig.cpp',
            'tools/idk-webview/main.cpp',
            'tools/idk-webview/manager.cpp',
            'tools/idk-webview/rhi_texture_extractor.cpp',
            'tools/idk-webview/utils.cpp',
            'tools/idk-webview/webview.cpp',
        )

        webview_headers = files(
            'tools/idk-webview/groupconfig.h',
            'tools/idk-webview/manager.h',
            'tools/idk-webview/rhi_texture_extractor.h',
            'tools/idk-webview/utils.h',
            'tools/idk-webview/webview.h',
        )

        idk_webview_moc = qt.preprocess(moc_headers: webview_headers, dependencies: qt6_dep)

        idk_webview_exe = executable(
            'idk-webview',
            webview_files + idk_webview_moc,
            dependencies: [qt6_dep, egl_dep2, idk_fs_dep],
            install: true,
        )
    else
        warning('Qt6 not found — idk-webview will not be built')
    endif
endif

message('\n=== idk-overlay build configuration ===')
message('  libidk-shim.so:       dlsym interceptor (LD_PRELOAD)')
message('  libidk-gl.so:         GL/EGL hooks + compositor (loaded by shim)')
message('  libidk-overlay.so:    inject path (EGL + Vulkan hooks)')
message('  libidk-framesource:   shared library (frame sender)')
message('  idk-webview:          Qt6 webview (DMA-BUF + idk socket)')
message('  idk-render:           render process (receive + read pixels)')
message('  idk-send-frame:       CLI (links libidk-framesource)')
message('  idk-inject:           CLI (wraps syringe_inject)')
message('=============================================\n')
