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

inc = include_directories('include')

vk_found = cc.has_function('vkCreateInstance', prefix: '#include <vulkan/vulkan.h>')

# ── Subprojects ────────────────────────────────────────────────────────

subdir('src/shaders')
subdir('src/shim')
subdir('src/render')
subdir('src/lib')

# ── libidk-gl.so — GL/EGL hooks + compositor (LD_PRELOAD path) ─────────
egl_dep = dependency('egl', required: false)
gles2_dep = dependency('glesv2', required: false)
if egl_dep.found() and gles2_dep.found()
    idk_gl_so = library(
        'idk-gl',
        [
            'src/shim/idk_gl_hook.c',
            'src/core/compositor.c',
            'src/gl/gl_loader.c',
            'src/gl/shader_loader.c',
            glsl_o_files,
        ],
        c_args: overlay_c_flags,
        include_directories: inc,
        dependencies: [egl_dep, gles2_dep],
        link_args: ['-ldl', '-lpthread'],
        objects: spv_o_files,
        version: '0.1',
        install: true,
    )
else
    warning('EGL/GLES2 dev headers not found — libidk-gl.so will not be built.')
endif

# ── libidk-overlay.so — inject path (EGL + Vulkan hooks) ───────────────
idk_overlay_so = library(
    'idk-overlay',
    [
        'src/hook/overlay.c',
        'src/gl/egl_hook.c',
        'src/core/compositor.c',
        'src/gl/gl_loader.c',
        'src/gl/shader_loader.c',
        'src/hook/vulkan_hook.c',
        'src/ipc/ipc.c',
        glsl_o_files,
    ],
    c_args: overlay_c_flags,
    include_directories: inc,
    dependencies: [syringe_dep, egl_dep],
    link_args: ['-ldl', '-lpthread'],
    objects: spv_o_files,
    version: '0.1',
    install: true,
)

# ── CLI targets ────────────────────────────────────────────────────────
if not meson.is_subproject()
    subdir('src/cli')
endif

# ── Build summary ──────────────────────────────────────────────────────
message('\n=== idk-overlay build configuration ===')
message('  libidk-shim.so:       dlsym interceptor (LD_PRELOAD)')
message('  libidk-gl.so:         GL/EGL hooks + compositor (loaded by shim)')
message('  libidk-overlay.so:    inject path (EGL + Vulkan hooks)')
message('  libidk-framesource:   shared library (frame sender)')
message('  idk-webview:          Qt6 webview (DMA-BUF + idk socket)')
message('  idk-render:           render process (receive + read pixels)')
message('  idk-inject:           CLI (wraps syringe_inject)')
message('=============================================\n')
