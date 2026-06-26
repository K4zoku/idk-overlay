# GLSL + SPIR-V shader embedding pipeline
# Variables set: glsl_o_files, spv_o_files

shaders = [
    ['overlay_120.vert', 'vertex_120', 'vert'],
    ['overlay_120.frag', 'fragment_120', 'frag'],
    ['overlay_130.vert', 'vertex_130', 'vert'],
    ['overlay_130.frag', 'fragment_130', 'frag'],
    ['overlay_300_es.vert', 'vertex_300_es', 'vert'],
    ['overlay_300_es.frag', 'fragment_300_es', 'frag'],
    ['overlay_410.vert', 'vertex_410', 'vert'],
    ['overlay_410.frag', 'fragment_410', 'frag'],
]

shader_dir = meson.current_source_dir()

sed     = find_program('sed')
ld      = find_program('ld')
objcopy = find_program('objcopy')
glslc   = find_program('glslc', required: false)
cp      = find_program('cp')
python  = find_program('python3')

build_rel = meson.current_build_dir().replace(meson.project_build_root() + '/', '')
glsl_o_files = []
spv_o_files = []

foreach s : shaders
    fname = s[0]
    variant = s[1]
    stage = s[2]

    glsl_sym = 'glsl_overlay_' + variant

    shader_copy = configure_file(
        input: shader_dir / fname,
        output: fname,
        copy: true,
    )

    body_file = custom_target(
        'strip_' + glsl_sym,
        input: shader_copy,
        output: glsl_sym + '_body.txt',
        command: [sed, '-n', '2,$w @OUTPUT@', '@INPUT@'],
        build_by_default: true,
    )

    body_bin = glsl_sym + '_body.txt'
    old_safe = (build_rel / body_bin).replace('.', '_').replace('-', '_').replace('/', '_')
    old_start = '_binary_' + old_safe + '_start'
    old_size = '_binary_' + old_safe + '_size'

    glsl_raw = custom_target(
        'ld_' + glsl_sym,
        input: body_file,
        output: glsl_sym + '_raw.o',
        command: [
            ld,
            '-r',
            '-b', 'binary',
            '-z', 'noexecstack',
            '@INPUT@',
            '-o', '@OUTPUT@',
        ],
        build_by_default: true,
    )

    glsl_o_files += custom_target(
        'obj_' + glsl_sym,
        input: glsl_raw,
        output: glsl_sym + '.o',
        command: [
            objcopy,
            '--redefine-sym', old_start + '=' + glsl_sym,
            '--redefine-sym', old_size + '=' + glsl_sym + '_size',
            '@INPUT@',
            '@OUTPUT@',
        ],
        build_by_default: true,
    )
endforeach

# ── SPIR-V pipeline (per-version-pair) ──
spv_pairs = [
    ['120', 'overlay_120.vert', 'overlay_120.frag'],
    ['130', 'overlay_130.vert', 'overlay_130.frag'],
    ['300_es', 'overlay_300_es.vert', 'overlay_300_es.frag'],
    ['410', 'overlay_410.vert', 'overlay_410.frag'],
]

foreach pair : spv_pairs
    ver = pair[0]
    vfile = pair[1]
    ffile = pair[2]
    vstage = 'vert'
    fstage = 'frag'

    spv_ok = false
    if glslc.found()
        rv = run_command(
            glslc,
            '-c', shader_dir / vfile,
            '-o', '/dev/null',
            '-fshader-stage=' + vstage,
            check: false,
        )
        rf = run_command(
            glslc,
            '-c', shader_dir / ffile,
            '-o', '/dev/null',
            '-fshader-stage=' + fstage,
            check: false,
        )
        spv_ok = rv.returncode() == 0 and rf.returncode() == 0
    endif

    if spv_ok
        vcopy = configure_file(input: shader_dir / vfile, output: vfile, copy: true)
        fcopy = configure_file(input: shader_dir / ffile, output: ffile, copy: true)

        vs_sym = 'spv_vertex_' + ver
        spv_v = custom_target(
            'spv_' + vs_sym,
            input: vcopy,
            output: vs_sym + '.spv',
            command: [
                glslc,
                '-c',
                '-fshader-stage=' + vstage,
                '@INPUT@',
                '-o', '@OUTPUT@',
            ],
            build_by_default: true,
        )
        vclean = custom_target(
            'spvc_' + vs_sym,
            input: spv_v,
            output: vs_sym + '.bin',
            command: [cp, '@INPUT@', '@OUTPUT@'],
            build_by_default: true,
        )
        vbin = vs_sym + '.bin'
        vsafe = (build_rel / vbin).replace('.', '_').replace('-', '_').replace('/', '_')
        vold_start = '_binary_' + vsafe + '_start'
        vold_size = '_binary_' + vsafe + '_size'
        vraw = custom_target(
            'ld_' + vs_sym,
            input: vclean,
            output: vs_sym + '_raw.o',
            command: [
                ld,
                '-r',
                '-b', 'binary',
                '-z', 'noexecstack',
                '@INPUT@',
                '-o', '@OUTPUT@',
            ],
            build_by_default: true,
        )
        spv_o_files += custom_target(
            'obj_' + vs_sym,
            input: vraw,
            output: vs_sym + '.o',
            command: [
                objcopy,
                '--redefine-sym', vold_start + '=' + vs_sym,
                '--redefine-sym', vold_size + '=' + vs_sym + '_size',
                '@INPUT@',
                '@OUTPUT@',
            ],
            build_by_default: true,
        )

        fs_sym = 'spv_fragment_' + ver
        spv_f = custom_target(
            'spv_' + fs_sym,
            input: fcopy,
            output: fs_sym + '.spv',
            command: [
                glslc,
                '-c',
                '-fshader-stage=' + fstage,
                '@INPUT@',
                '-o', '@OUTPUT@',
            ],
            build_by_default: true,
        )
        fclean = custom_target(
            'spvc_' + fs_sym,
            input: spv_f,
            output: fs_sym + '.bin',
            command: [cp, '@INPUT@', '@OUTPUT@'],
            build_by_default: true,
        )
        fbin = fs_sym + '.bin'
        fsafe = (build_rel / fbin).replace('.', '_').replace('-', '_').replace('/', '_')
        fold_start = '_binary_' + fsafe + '_start'
        fold_size = '_binary_' + fsafe + '_size'
        fraw = custom_target(
            'ld_' + fs_sym,
            input: fclean,
            output: fs_sym + '_raw.o',
            command: [
                ld,
                '-r',
                '-b', 'binary',
                '-z', 'noexecstack',
                '@INPUT@',
                '-o', '@OUTPUT@',
            ],
            build_by_default: true,
        )
        spv_o_files += custom_target(
            'obj_' + fs_sym,
            input: fraw,
            output: fs_sym + '.o',
            command: [
                objcopy,
                '--redefine-sym', fold_start + '=' + fs_sym,
                '--redefine-sym', fold_size + '=' + fs_sym + '_size',
                '@INPUT@',
                '@OUTPUT@',
            ],
            build_by_default: true,
        )

        add_project_arguments('-DHAS_SPV_' + ver, language: 'c')
        message('SPIR-V enabled for version @0@'.format(ver))
    else
        message('SPIR-V disabled for version @0@'.format(ver))
    endif
endforeach

# ── Vulkan overlay shaders (overlay_vk.vert/frag) ───────────────────────
# Compiled to SPIR-V and embedded as object files via ld+objcopy (same flow
# as the GL SPIR-V shaders above). This produces symbols:
#   spv_overlay_vk_vert, spv_overlay_vk_vert_size
#   spv_overlay_vk_frag, spv_overlay_vk_frag_size
# declared extern in include/shaders/vk_shaders.h and linked via spv_o_files.

vk_shader_dir = meson.current_source_dir()
vk_build_dir = meson.current_build_dir()

vk_spv_ok = false
if glslc.found()
    rvk = run_command(
        glslc,
        '-c', vk_shader_dir / 'overlay_vk.vert',
        '-o', '/dev/null',
        '-fshader-stage=vert',
        check: false,
    )
    rfk = run_command(
        glslc,
        '-c', vk_shader_dir / 'overlay_vk.frag',
        '-o', '/dev/null',
        '-fshader-stage=frag',
        check: false,
    )
    vk_spv_ok = rvk.returncode() == 0 and rfk.returncode() == 0
endif

if vk_spv_ok
    vk_vcopy = configure_file(input: vk_shader_dir / 'overlay_vk.vert', output: 'overlay_vk.vert', copy: true)
    vk_fcopy = configure_file(input: vk_shader_dir / 'overlay_vk.frag', output: 'overlay_vk.frag', copy: true)

    # Vertex: glslc → .spv → .bin → ld -b binary → objcopy rename
    vk_vs_sym = 'spv_overlay_vk_vert'
    vk_spv_v = custom_target('spv_overlay_vk_vert',
        input: vk_vcopy,
        output: vk_vs_sym + '.spv',
        command: [glslc, '-c', '-fshader-stage=vert', '@INPUT@', '-o', '@OUTPUT@'],
        build_by_default: true,
    )
    vk_vclean = custom_target('spvc_overlay_vk_vert',
        input: vk_spv_v,
        output: vk_vs_sym + '.bin',
        command: [cp, '@INPUT@', '@OUTPUT@'],
        build_by_default: true,
    )
    vk_vbin = vk_vs_sym + '.bin'
    vk_vsafe = (build_rel / vk_vbin).replace('.', '_').replace('-', '_').replace('/', '_')
    vk_vold_start = '_binary_' + vk_vsafe + '_start'
    vk_vold_size = '_binary_' + vk_vsafe + '_size'
    vk_vraw = custom_target('ld_overlay_vk_vert',
        input: vk_vclean,
        output: vk_vs_sym + '_raw.o',
        command: [ld, '-r', '-b', 'binary', '-z', 'noexecstack', '@INPUT@', '-o', '@OUTPUT@'],
        build_by_default: true,
    )
    spv_o_files += custom_target('obj_overlay_vk_vert',
        input: vk_vraw,
        output: vk_vs_sym + '.o',
        command: [
            objcopy,
            '--redefine-sym', vk_vold_start + '=' + vk_vs_sym,
            '--redefine-sym', vk_vold_size + '=' + vk_vs_sym + '_size',
            '@INPUT@', '@OUTPUT@',
        ],
        build_by_default: true,
    )

    # Fragment: same flow
    vk_fs_sym = 'spv_overlay_vk_frag'
    vk_spv_f = custom_target('spv_overlay_vk_frag',
        input: vk_fcopy,
        output: vk_fs_sym + '.spv',
        command: [glslc, '-c', '-fshader-stage=frag', '@INPUT@', '-o', '@OUTPUT@'],
        build_by_default: true,
    )
    vk_fclean = custom_target('spvc_overlay_vk_frag',
        input: vk_spv_f,
        output: vk_fs_sym + '.bin',
        command: [cp, '@INPUT@', '@OUTPUT@'],
        build_by_default: true,
    )
    vk_fbin = vk_fs_sym + '.bin'
    vk_fsafe = (build_rel / vk_fbin).replace('.', '_').replace('-', '_').replace('/', '_')
    vk_fold_start = '_binary_' + vk_fsafe + '_start'
    vk_fold_size = '_binary_' + vk_fsafe + '_size'
    vk_fraw = custom_target('ld_overlay_vk_frag',
        input: vk_fclean,
        output: vk_fs_sym + '_raw.o',
        command: [ld, '-r', '-b', 'binary', '-z', 'noexecstack', '@INPUT@', '-o', '@OUTPUT@'],
        build_by_default: true,
    )
    spv_o_files += custom_target('obj_overlay_vk_frag',
        input: vk_fraw,
        output: vk_fs_sym + '.o',
        command: [
            objcopy,
            '--redefine-sym', vk_fold_start + '=' + vk_fs_sym,
            '--redefine-sym', vk_fold_size + '=' + vk_fs_sym + '_size',
            '@INPUT@', '@OUTPUT@',
        ],
        build_by_default: true,
    )

    add_project_arguments('-DHAS_VK_SPV', language: 'c')
    message('Vulkan overlay shaders: SPIR-V enabled (object embedding)')
else
    message('Vulkan overlay shaders: glslc not found — VK pipeline will be unavailable at runtime')
endif
