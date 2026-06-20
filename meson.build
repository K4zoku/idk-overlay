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

overlay_c_flags = [
    '-D_GNU_SOURCE',
    '-Wall',
    '-Wextra',
    '-O2',
]

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
    'src/idk_shim.c',
    c_args: overlay_c_flags,
    include_directories: include_directories('include'),
    link_args: ['-ldl', '-lpthread'],
    version: '0.1',
    install: true,
)

# ── libidk-gl.so — GL/EGL hooks + compositor ────────────────────────────
# Loaded by libidk-shim.so at runtime. Provides actual GL hooks:
#   - idk_eglSwapBuffers()   — render overlay after real swap
#   - idk_eglGetProcAddress() — resolve GL functions + return hooks
#   - idk_eglGetDisplay()     — wrapper around real eglGetDisplay
#   - idk_eglGetPlatformDisplay() — wrapper around real eglGetPlatformDisplay
#   - idk_eglTerminate()      — wrapper around real eglTerminate
#   - idk_eglDestroyContext() — wrapper around real eglDestroyContext
idk_gl_so = library(
    'idk-gl',
    [
        'src/idk_gl_hook.c',
        'src/compositor.c',
    ],
    c_args: overlay_c_flags,
    include_directories: include_directories('include'),
    link_args: ['-ldl', '-lpthread', '-lEGL', '-lGL'],
    version: '0.1',
    install: true,
)

# ── libidk-overlay.so — Vulkan hooks + main init (for Vulkan games) ─────
# Simplified: no longer includes GL hooks (moved to libidk-gl.so)
# Used for Vulkan-based games that don't use GL/EGL.
idk_overlay_so = library(
    'idk-overlay',
    [
        'src/overlay.c',
        'src/vulkan_hook.c',
        'src/ipc.c',
    ],
    c_args: overlay_c_flags,
    include_directories: include_directories('include'),
    link_args: ['-ldl', '-lpthread'],
    dependencies: [syringe_dep],
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
    subproject('idk-client-qt', default_options: ['c_std=gnu11'])
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
