# libidk-producer.so - frame sender library (used by idk-webview)
idk_producer_lib = library(
    'idk-producer',
    ['api.c', '../core/log.c'] + transport_sources,
    c_args: fs_c_flags,
    include_directories: inc,
    link_args: ['-ldl', '-lpthread'],
    version: '0.1',
    install: true,
)

idk_producer_dep = declare_dependency(
    include_directories: inc,
    link_with: idk_producer_lib,
)
