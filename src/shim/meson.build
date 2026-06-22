# libidk-shim.so — dlsym interceptor (LD_PRELOAD first)
idk_shim_so = library(
    'idk-shim',
    ['idk_shim.c', 'elfhacks.c'],
    c_args: overlay_c_flags,
    include_directories: inc,
    link_args: ['-ldl', '-lpthread'],
    version: '0.1',
    install: true,
)
