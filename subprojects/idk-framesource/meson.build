project('idk-framesource', 'c', version: '0.1.0')

fs_c_flags = ['-D_GNU_SOURCE', '-Wall', '-Wextra', '-O2']

idk_overlay_include = include_directories('../../include')
idk_public_include = include_directories('../../include/public')

idk_fs_lib = shared_library(
    'idk-framesource',
    'src/idk_fs.c',
    c_args: fs_c_flags,
    include_directories: [include_directories('include'), idk_overlay_include, idk_public_include],
    link_args: ['-ldl', '-lpthread'],
    version: '0.1',
    install: true,
)

idk_fs_dep = declare_dependency(
    include_directories: [include_directories('include'), idk_overlay_include, idk_public_include],
    link_with: idk_fs_lib,
)

message('  libidk-framesource: shared library (frame sender)')
