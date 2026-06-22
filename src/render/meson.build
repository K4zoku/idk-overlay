# idk-render — render process (receive + read pixels)
idk_render_exe = executable(
    'idk-render',
    ['render.c', 'receive.c', '../ipc/ipc.c'],
    c_args: overlay_c_flags,
    include_directories: inc,
    link_args: ['-ldl', '-lpthread'],
    dependencies: [syringe_dep, pthread_lib],
    install: true,
)
