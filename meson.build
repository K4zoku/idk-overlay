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

idk_client_proj = subproject('idk-client')
idk_client_dep = idk_client_proj.get_variable('idk_client_dep')

overlay_c_flags = ['-D_GNU_SOURCE', '-Wall', '-Wextra', '-O2', '-Wno-pedantic', '-fno-stack-protector']

# ── Shader embedding ─────────────────────────────────────────────────────
# Generate overlay_shader_embed.h from shaders/*.vert/.frag at build time.
# The output goes to the build directory; Meson automatically adds it to
# include paths when listed as a source in a target.
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
    # Fallback: use the committed copy in the source tree
    overlay_shader_embed_h = files('src/overlay_shader_embed.h')
endif

vk_found = cc.has_function('vkCreateInstance', prefix: '#include <vulkan/vulkan.h>')
if vk_found
    message('  → Vulkan support: enabled')
else
    message('  → Vulkan support: NOT FOUND (dmabuf export will be unavailable)')
endif

# ── libidk-shim.so — dlsym interceptor (LD_PRELOAD first) ───────────────
# Acts as the first library loaded. Intercepts dlsym() to redirect GL/EGL
# function lookups to our hooks. Loaded via LD_PRELOAD before the game.
idk_shim_so = library(
    'idk-shim',
    ['src/idk_shim.c', 'src/elfhacks.c'],
    c_args: overlay_c_flags,
    include_directories: include_directories('include'),
    link_args: ['-ldl', '-lpthread'],
    version: '0.1',
    install: true,
)

# ── libidk-gl.so — GL/EGL hooks + compositor (LD_PRELOAD path) ──────────
# Loaded by libidk-shim.so at runtime. Provides actual GL hooks:
#   - idk_eglSwapBuffers()   — render overlay after real swap
#   - idk_eglGetProcAddress() — resolve GL functions + return hooks
#   - idk_eglGetDisplay()     — wrapper around real eglGetDisplay
#   - idk_eglGetPlatformDisplay() — wrapper around real eglGetPlatformDisplay
#   - idk_eglTerminate()      — wrapper around real eglTerminate
#   - idk_eglDestroyContext() — wrapper around real eglDestroyContext
#
# Optional — only built when real EGL + GLES2 dev headers are available
# (libegl1-mesa-dev + libgles2-mesa-dev). Skip otherwise; the ptrace
# inject path (libidk-overlay.so + egl_hook.c) doesn't need this.
egl_dep = dependency('egl', required: false)
gles2_dep = dependency('glesv2', required: false)
if egl_dep.found() and gles2_dep.found()
    idk_gl_so = library(
        'idk-gl',
        [
            'src/idk_gl_hook.c',
            'src/compositor.c',
            'src/gl_loader.c',
            overlay_shader_embed_h,
        ],
        c_args: overlay_c_flags,
        include_directories: include_directories('include'),
        dependencies: [egl_dep, gles2_dep],
        link_args: ['-ldl', '-lpthread'],
        version: '0.1',
        install: true,
    )
else
    warning(
        'EGL/GLES2 dev headers not found — libidk-gl.so (LD_PRELOAD path) will not be built. Install libegl1-mesa-dev + libgles2-mesa-dev to enable.',
    )
endif

# ── libidk-overlay.so — Vulkan + EGL hooks + main init ───────────────────
# Injected into target process via syringe_inject(). Constructor auto-detects
# which graphics API the target uses (Vulkan via vkQueuePresentKHR hook,
# EGL via eglSwapBuffers hook using install_addr).
#
# EGL path (osu-lazer / Wayland / OpenGL):
#   egl_hook.c   — hooks eglSwapBuffers via syringe_hook_install_addr
#   compositor.c — receives DMA-BUF from webview, renders fullscreen quad
#
# Vulkan path:
#   vulkan_hook.c — hooks vkQueuePresentKHR
#
# Note: compositor.c uses GL functions but they are resolved at runtime
# via dlsym (see src/gl_loader.c, include/idk_gl_loader.h). No link-time
# dependency on libGL/libGLESv2.
egl_dep2 = dependency('egl', required: false)
idk_overlay_so = library(
    'idk-overlay',
    [
        'src/overlay.c',
        'src/egl_hook.c',
        'src/compositor.c',
        'src/gl_loader.c',
        'src/vulkan_hook.c',
        'src/ipc.c',
        overlay_shader_embed_h,
    ],
    c_args: overlay_c_flags,
    include_directories: include_directories('include'),
    dependencies: [syringe_dep, egl_dep2],
    link_args: ['-ldl', '-lpthread'],
    version: '0.1',
    install: true,
)

# ── idk-render ───────────────────────────────────────────────────────────
idk_render_exe = executable(
    'idk-render',
    [
        'src/render.c',
        'src/ipc.c',
    ],
    c_args: overlay_c_flags,
    include_directories: include_directories('include'),
    link_args: ['-ldl', '-lpthread'],
    dependencies: [syringe_dep, pthread_lib],
    install: true,
)

# ── idk-inject ───────────────────────────────────────────────────────────
idk_inject_exe = executable(
    'idk-inject',
    'src/inject.c',
    c_args: overlay_c_flags,
    link_args: ['-ldl', '-lpthread'],
    dependencies: [syringe_dep],
    install: true,
)

# ── idk-client ───────────────────────────────────────────────────────────
# Standalone binary for sending overlay frames from external processes
# (e.g., webview client adapted from imgoverlay client).
# Links against libidk-client from subprojects/idk-client/.
# Connects to the idk-overlay socket and sends frames using the same
# wire protocol as the injected library (frame header + fd via SCM_RIGHTS).
idk_client_exe = executable(
    'idk-client',
    'src/idk-client.c',
    c_args: overlay_c_flags,
    include_directories: include_directories('include'),
    link_args: ['-ldl', '-lpthread'],
    dependencies: [idk_client_dep, pthread_lib],
    install: true,
)

# ── idk-client-qt (optional — requires Qt6WebEngine) ─────────────────────
# Qt6 webview client — renders web pages and sends frames to idk-overlay
# via DMA-BUF (Qt6 RHI) or SHM fallback. Adapted from imgoverlay client.
if not meson.is_subproject()
    qt6_dep = dependency(
        'qt6',
        modules: ['Core', 'Gui', 'QuickWidgets', 'WebEngineWidgets', 'Widgets'],
        required: false,
    )
    if qt6_dep.found()
        subproject('idk-client-qt', default_options: ['c_std=gnu11'])
    else
        warning('Qt6 not found — idk-client-qt (webview) will not be built')
    endif
endif
message('\n=== idk-overlay build configuration ===')
message('  libidk-shim.so:       dlsym interceptor (LD_PRELOAD first)')
message('  libidk-gl.so:         GL/EGL hooks + compositor (loaded by shim)')
message('  libidk-overlay.so:    Vulkan hooks + main init (Vulkan games)')
message('  libidk-client:        shared library (frame sender)')
message('  idk-client-qt:        Qt6 webview (DMA-BUF + idk socket)')
message('  idk-render:           render process (receive + read pixels)')
message('  idk-client:           client binary (links libidk-client)')
message('  idk-inject:           CLI (wraps syringe_inject)')
message('===================================================\n')
