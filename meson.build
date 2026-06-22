project(
    'idk-overlay',
    'c',
    version: '0.1.0',
    default_options: [
        'warning_level=3',
        'c_std=gnu11',
    ],
    meson_version: '>= 1.0.0',
)

add_project_arguments('-fPIC', language: 'c')

cc = meson.get_compiler('c')
pthread_lib = cc.find_library('pthread', required: true)
dl_lib = cc.find_library('dl', required: true)

syringe_proj = subproject('syringe')
syringe_dep = syringe_proj.get_variable('libsyringe_dep')

idk_fs_proj = subproject('idk-framesource')
idk_fs_dep = idk_fs_proj.get_variable('idk_fs_dep')

overlay_c_flags = ['-D_GNU_SOURCE', '-Wall', '-Wextra', '-O2', '-Wno-pedantic', '-fno-stack-protector']

# ── Include directories ────────────────────────────────────────────────────
inc = include_directories('include')
inc_public = include_directories('include/public')

# ── Shader embedding ───────────────────────────────────────────────────────
python3 = find_program('python3', required: false)
shader_srcs = files(
    'shaders/overlay_120.vert',
    'shaders/overlay_120.frag',
    'shaders/overlay_130.vert',
    'shaders/overlay_130.frag',
    'shaders/overlay_300_es.vert',
    'shaders/overlay_300_es.frag',
    'shaders/overlay_410.vert',
    'shaders/overlay_410.frag',
)
if python3.found()
    overlay_shader_embed_h = custom_target('embed-shaders',
        input: shader_srcs,
        output: 'overlay_shader_embed.h',
        command: [python3, meson.current_source_dir() / 'scripts/embed_shaders.py',
                  '--output', '@OUTPUT@',
                  '--shader-dir', meson.current_source_dir() / 'shaders'],
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

# ── idk-send-frame (optional CLI — links libidk-framesource) ───────────────
idk_send_frame_exe = executable(
    'idk-send-frame',
    'src/framesource/idk-send-frame.c',
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
    )
    if qt6_dep.found()
        subproject('idk-webview', default_options: ['c_std=gnu11'])
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
