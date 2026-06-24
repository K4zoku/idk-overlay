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

egl_dep = cc.find_library('EGL', required: false)
gl_dep = cc.find_library('OpenGL', required: false)
vk_dep = cc.find_library('vulkan', required: false)

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

vk_header_found = cc.has_header('vulkan/vulkan.h')
if vk_header_found
    vk_test = cc.links(
        '''
        #include <vulkan/vulkan.h>
        int main(void) { VkInstance inst; vkCreateInstance(NULL, NULL, &inst); return 0; }
        ''',
        name: 'vkCreateInstance',
        dependencies: [vk_dep],
    )
    if vk_test
        add_project_arguments('-DIDK_HAVE_VULKAN', language: 'c')
        add_project_arguments('-DIDK_HAVE_VULKAN', language: 'cpp')
    endif
endif

# ── Subprojects ────────────────────────────────────────────────────────

subdir('src/shaders')
subdir('src/render')
subdir('src/lib')

# ── libidk-overlay.so — inject path (EGL + Vulkan hooks) ───────────────
idk_overlay_so = library(
    'idk-overlay',
    [
        'src/hook/overlay.c',
        'src/hook/egl_hook.c',
        'src/hook/glx_hook.c',
        'src/hook/vulkan_hook.c',
        'src/hook/wayland_input.c',
        'src/core/compositor.c',
        'src/gl/gl_loader.c',
        'src/gl/shader_loader.c',
        'src/ipc/ipc.c',
        'src/shim/elfhacks.c',
        glsl_o_files,
    ],
    c_args: overlay_c_flags,
    include_directories: inc,
    dependencies: [syringe_dep, egl_dep, vk_dep],
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
message('  libidk-overlay.so:    inject path (EGL + Vulkan hooks)')
message('  libidk-framesource:   shared library (frame sender)')
message('  idk-webview:          Qt6 webview (DMA-BUF + idk socket)')
message('  idk-render:           render process (receive + read pixels)')
message('  idk-inject:           CLI (wraps syringe_inject)')
message('=============================================\n')
