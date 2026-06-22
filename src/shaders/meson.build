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
