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
# Public headers accessed via #include "public/idk_xxx.h"
# Internal headers via #include "core/...", "gl/...", "inject/...", etc.

# ── Shader embedding ───────────────────────────────────────────────────────
#
# Pipeline for EACH shader:
#   1. configure_file(copy:true) → copy to build dir (tên sạch)
#   2. sed '2,$w'              → strip #version line (compositor prepends at runtime)
#   3. ld -r -b binary         → embed text as ELF .o
#   4. objcopy --redefine-sym  → rename auto-generated symbols to clean names
#
# If glslc is found, SPIR-V objects are also built:
#   5. glslc -c                → compile GLSL → SPIR-V
#   6. ld -r -b binary         → embed SPIR-V as ELF .o
#   7. objcopy --redefine-sym  → rename symbols

fs   = import('fs')
sed  = find_program('sed')
ld   = find_program('ld')
objcopy = find_program('objcopy')
glslc = find_program('glslc', required: false)
cp    = find_program('cp')

shader_dir = meson.current_source_dir() / 'src/shaders'

# [filename, variant_label, glslc_stage]
shaders = [
  ['overlay_120.vert',   'vertex_120',   'vert'],
  ['overlay_120.frag',   'fragment_120', 'frag'],
  ['overlay_130.vert',   'vertex_130',   'vert'],
  ['overlay_130.frag',   'fragment_130', 'frag'],
  ['overlay_300_es.vert','vertex_300_es','vert'],
  ['overlay_300_es.frag','fragment_300_es','frag'],
  ['overlay_410.vert',   'vertex_410',   'vert'],
  ['overlay_410.frag',   'fragment_410', 'frag'],
]

glsl_o_files = []
spv_o_files = []

foreach s : shaders
  fname   = s[0]
  variant = s[1]
  stage   = s[2]

  glsl_sym = 'glsl_overlay_' + variant
  spv_sym  = 'spv_' + variant

  # ── Step 1: Copy shader → build dir ──
  shader_copy = configure_file(
    input: shader_dir / fname,
    output: fname,
    copy: true,
  )

  # Compute ld -b binary old symbol names from the absolute build path
  fpath = shader_copy.full_path()
  safe  = fpath.replace('/', '_').replace('.', '_').replace('-', '_')
  old_start = '_binary_' + safe + '_start'
  old_size  = '_binary_' + safe + '_size'

  # ── Step 2: Strip #version with sed ──
  body_file = custom_target('strip_' + glsl_sym,
    input: shader_copy,
    output: glsl_sym + '_body.txt',
    command: [sed, '-n', '2,$w @OUTPUT@', '@INPUT@'],
    build_by_default: true,
  )

  # ── Step 3: ld -b binary → raw .o (symbols based on @INPUT@ path) ──
  glsl_raw = custom_target('ld_' + glsl_sym,
    input: body_file,
    output: glsl_sym + '_raw.o',
    command: [ld, '-r', '-b', 'binary', '-z', 'noexecstack',
              '@INPUT@', '-o', '@OUTPUT@'],
    build_by_default: true,
  )

  # ── Step 4: objcopy --redefine-sym → clean symbol names ──
  glsl_o_files += custom_target('obj_' + glsl_sym,
    input: glsl_raw,
    output: glsl_sym + '.o',
    command: [objcopy,
      '--redefine-sym', old_start + '=' + glsl_sym,
      '--redefine-sym', old_size  + '=' + glsl_sym + '_size',
      '@INPUT@', '@OUTPUT@'],
    build_by_default: true,
  )

  # ── Step 5-7: SPIR-V pipeline (only if glslc found) ──
  if glslc.found()
    # Step 5: glslc compile
    spv_file = custom_target('spv_' + spv_sym,
      input: shader_copy,
      output: spv_sym + '.spv',
      command: [glslc, '-c', '-fshader-stage=' + stage,
                '@INPUT@', '-o', '@OUTPUT@'],
      build_by_default: true,
    )

    # Step 5b: Copy .spv to build root so ld symbol names are clean
    spv_clean = custom_target('spvc_' + spv_sym,
      input: spv_file,
      output: spv_sym + '.bin',
      command: [cp, '@INPUT@', '@OUTPUT@'],
      build_by_default: true,
    )

    # Compute SPIR-V old symbol names
    spv_fpath = spv_clean.full_path()
    spv_safe  = spv_fpath.replace('/', '_').replace('.', '_').replace('-', '_')
    spv_old_start = '_binary_' + spv_safe + '_start'
    spv_old_size  = '_binary_' + spv_safe + '_size'

    # Step 6: ld -b binary
    spv_raw = custom_target('ld_' + spv_sym,
      input: spv_clean,
      output: spv_sym + '_raw.o',
      command: [ld, '-r', '-b', 'binary', '-z', 'noexecstack',
                '@INPUT@', '-o', '@OUTPUT@'],
      build_by_default: true,
    )

    # Step 7: objcopy rename
    spv_o_files += custom_target('obj_' + spv_sym,
      input: spv_raw,
      output: spv_sym + '.o',
      command: [objcopy,
        '--redefine-sym', spv_old_start + '=' + spv_sym,
        '--redefine-sym', spv_old_size  + '=' + spv_sym + '_size',
        '@INPUT@', '@OUTPUT@'],
      build_by_default: true,
    )
  endif
endforeach

if glslc.found()
  add_project_arguments('-DHAS_SPIRV', language: 'c')
  message('SPIR-V shader compilation enabled (glslc found)')
else
  message('SPIR-V shader compilation disabled (glslc not found, GLSL fallback only)')
endif

vk_found = cc.has_function('vkCreateInstance', prefix: '#include <vulkan/vulkan.h>')

# ── libidk-shim.so — dlsym interceptor (LD_PRELOAD first) ─────────────────
idk_shim_so = library(
    'idk-shim',
    ['src/shim/idk_shim.c', 'src/shim/elfhacks.c'],
    c_args: overlay_c_flags,
    include_directories: [inc],
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
        glsl_o_files,
    ],
    c_args: overlay_c_flags,
    include_directories: [inc],
    dependencies: [egl_dep, gles2_dep],
    link_args: ['-ldl', '-lpthread'],
    objects: spv_o_files,
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
        glsl_o_files,
    ],
    c_args: overlay_c_flags,
    include_directories: [inc],
    dependencies: [syringe_dep, egl_dep2],
    link_args: ['-ldl', '-lpthread'],
    objects: spv_o_files,
    version: '0.1',
    install: true,
)

# ── idk-render ─────────────────────────────────────────────────────────────
idk_render_exe = executable(
    'idk-render',
    ['src/render/render.c', 'src/render/receive.c', 'src/ipc/ipc.c'],
    c_args: overlay_c_flags,
    include_directories: [inc],
    link_args: ['-ldl', '-lpthread'],
    dependencies: [syringe_dep, pthread_lib],
    install: true,
)

# ── idk-inject ─────────────────────────────────────────────────────────────
idk_inject_exe = executable(
    'idk-inject',
    'src/inject/inject.c',
    c_args: overlay_c_flags,
    include_directories: [inc],
    link_args: ['-ldl', '-lpthread'],
    dependencies: [syringe_dep],
    install: true,
)

# ── libidk-framesource.so — frame sender library (used by idk-webview) ─────
idk_fs_lib = library(
    'idk-framesource',
    'src/lib/idk_fs.c',
    c_args: fs_c_flags,
    include_directories: [inc],
    link_args: ['-ldl', '-lpthread'],
    version: '0.1',
    install: true,
)

idk_fs_dep = declare_dependency(
    include_directories: [inc],
    link_with: idk_fs_lib,
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
message('  idk-inject:           CLI (wraps syringe_inject)')
message('=============================================\n')
