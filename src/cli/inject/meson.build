# idk-inject - CLI (wraps syringe_inject)
idk_inject_exe = executable(
    'idk-inject',
    'inject.c', '../../core/log.c',
    c_args: overlay_c_flags,
    include_directories: inc,
    link_args: ['-ldl', '-lpthread'],
    dependencies: [syringe_dep],
    install: true,
)
