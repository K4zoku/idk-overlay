project(
    'idk-overlay',
    ['c', 'cpp'],
    version: '0.1.0',
    default_options: [
        'warning_level=3',
        'c_std=gnu11',
    ],
    meson_version: '>= 1.1.0',
)

add_project_arguments('-fPIC', language: 'c')
add_project_arguments('-fPIC', language: 'cpp')

cc = meson.get_compiler('c')
pthread_lib = cc.find_library('pthread', required: true)
dl_lib = cc.find_library('dl', required: true)

egl_dep = cc.find_library('EGL', required: false)
gl_dep = cc.find_library('OpenGL', required: false)
vk_dep = cc.find_library('vulkan', required: false)

syringe_proj = subproject('syringe', default_options: ['default_library=static'])
syringe_dep = syringe_proj.get_variable('libsyringe_dep')

# Strip .git from the syringe subproject so editors don't show nested-repo
# status for patch-modified files.
run_command('rm', '-rf',
    meson.project_source_root() / 'subprojects/syringe/.git',
    check: false)

overlay_c_flags = [
    '-D_GNU_SOURCE',
    '-Wall',
    '-Wextra',
    '-O2',
    '-Wno-pedantic',
    '-fno-stack-protector',
]
fs_c_flags = ['-D_GNU_SOURCE', '-Wall', '-Wextra', '-O2']

inc = include_directories('include', 'src/shaders')

vk_header_found = cc.has_header('vulkan/vulkan.h')
vk_layer_header_found = cc.has_header('vulkan/vk_layer.h')
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
        # NOTE: -DIDK_HAVE_VK_LAYER is NOT global — only libidk-vklayer.so
        # compiles vulkan_layer.c. A global define would make wayland_input.c
        # reference idk_vk_layer_is_active() (defined in vulkan_layer.c) and
        # break the preload lib link.
    endif
endif

# ── Subprojects ────────────────────────────────────────────────────────

subdir('src/shaders')
subdir('src/lib')

# Sources shared by both the preload overlay and the Vulkan layer.
idk_core_src = [
    'src/core/log.c',
    'src/core/compositor.c',
    'src/core/broker.c',
    'src/core/transport.c',
    'src/core/tp_socket.c',
    'src/core/tp_shm.c',
]

# ── libidk-overlay.so - LD_PRELOAD/inject path (EGL + GLX hooks) ──────
# Has __attribute__((constructor)) on_load() in overlay.c — safe because
# this library is only ever loaded deliberately (LD_PRELOAD, idk-inject).
idk_overlay_so = library(
    'idk-overlay',
    idk_core_src + [
        'src/hook/overlay.c',
        'src/hook/dlsym_shim.c',
        'src/hook/egl_hook.c',
        'src/hook/glx_hook.c',
        'src/hook/wayland/wayland_input.c',
        'src/hook/wayland/wayland_ptr.c',
        'src/hook/wayland/wayland_kb.c',
        'src/hook/wayland/wayland_sidecar.c',
        'src/hook/wayland/wayland_socket.c',
        'src/hook/wayland/wayland_scan.c',
        'src/hook/x11/x11_input.c',
        'src/hook/x11/x11_kb.c',
        'src/hook/x11/x11_ptr.c',
        'src/core/compositor_egl.c',
        'src/gl/gl_loader.c',
        'src/gl/shader_loader.c',
        'src/ipc/ipc.c',
        glsl_o_files,
    ],
    c_args: overlay_c_flags,
    include_directories: inc,
    dependencies: [syringe_dep, egl_dep, vk_dep],
    link_args: ['-ldl', '-lpthread'],
    version: '0.1',
    install: true,
)

# ── libidk-vklayer.so - Vulkan layer (VK hooks only) ──────────────────
# NO constructor: the Vulkan loader dlopens this into *any* Vulkan app
# (vulkaninfo, vkcube, ...). Everything runs lazily from vkCreateInstance
# via layer_main.c's idk_layer_ensure_started().
vklayer_c_args = overlay_c_flags
if vk_layer_header_found
    vklayer_c_args += '-DIDK_HAVE_VK_LAYER'
endif
idk_vklayer_so = library(
    'idk-vklayer',
    idk_core_src + [
        'src/hook/vulkan_layer.c',
        'src/hook/layer_main.c',
        'src/core/compositor_vk.c',
    ],
    c_args: vklayer_c_args,
    include_directories: inc,
    dependencies: [vk_dep],
    link_args: ['-ldl', '-lpthread'],
    objects: spv_o_files,
    version: '0.1',
    install: true,
)

# ── Vulkan layer manifest ─────────────────────────────────────────────
# The JSON manifest tells the Vulkan loader about our layer.
# The layer is only enabled when IDK_VK_LAYER=1 env is set.
# For testing: VK_LAYER_PATH=<build_dir> IDK_VK_LAYER=1 vkcube
vulkan_layer_json = configure_file(
    input: 'src/hook/idk_overlay.json.in',
    output: 'idk_overlay.json',
    configuration: {
        'IDK_LAYER_LIB_PATH': meson.current_build_dir() / 'libidk-vklayer.so.0.1',
    },
)

# Install system-wide into implicit_layer.d so Vulkan loader discovers it
# automatically. The enable_environment/disable_environment keys in the JSON
# control activation — off by default, enabled via IDK_VK_LAYER=1.
vulkan_layer_json_install = configure_file(
    input: 'src/hook/idk_overlay.json.in',
    output: 'idk_overlay_system.json',
    configuration: {
        'IDK_LAYER_LIB_PATH': get_option('prefix') / get_option('libdir') / 'libidk-vklayer.so',
    },
)
install_data(
    vulkan_layer_json_install,
    install_dir: get_option('datadir') / 'vulkan/implicit_layer.d',
    rename: 'idk_overlay.json',
)

# ── CLI targets ────────────────────────────────────────────────────────
if not meson.is_subproject()
    subdir('src/cli')
endif

# ── Tests ──────────────────────────────────────────────────────────────
if not meson.is_subproject()
    subdir('tests')
endif

# ── Build summary ──────────────────────────────────────────────────────
message('\n=== idk-overlay build configuration ===')
message('  libidk-overlay.so:    inject path (EGL + Vulkan hooks)')
message('  libidk-framesource:   shared library (frame sender)')
message('  idk-webview:          Qt6 webview (DMA-BUF + idk socket)')
message('  idk-inject:           CLI (wraps syringe_inject)')
message('  idk-broker:           host-ns webview spawner (Wine/Proton)')
message('=============================================\n')
