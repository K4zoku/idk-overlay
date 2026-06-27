# libidk-framesource.so - frame sender library (used by idk-webview)
idk_fs_lib = library(
    'idk-framesource',
    ['idk_fs.c', '../core/transport.c', '../core/tp_socket.c', '../core/tp_shm.c'],
    c_args: fs_c_flags,
    include_directories: inc,
    link_args: ['-ldl', '-lpthread'],
    version: '0.1',
    install: true,
)

idk_fs_dep = declare_dependency(
    include_directories: inc,
    link_with: idk_fs_lib,
)
