test_c_args = [
    '-D_GNU_SOURCE',
    '-Wall',
    '-Wextra',
    '-O2',
    '-Wno-pedantic',
]

# Static library of pure-logic functions from compositor_common.c
# (no GPU dependencies - just stdlib + syscalls)
test_comp_common_lib = static_library('test_comp_common',
    '../src/core/compositor_common.c',
    c_args: test_c_args,
    include_directories: inc,
)

test_comp_common_dep = declare_dependency(
    link_with: test_comp_common_lib,
    include_directories: inc,
)

# Header-only tests (no linking needed)
foreach src : ['test_wire_protocol.c', 'test_input.c', 'test_ack.c']
    name = src.split('.')[0]
    test_exe = executable(name, src,
        c_args: test_c_args,
        include_directories: inc,
    )
    test(name, test_exe)
endforeach

# Tests needing compositor_common symbols
foreach src : ['test_vendor.c', 'test_resize.c', 'test_shm_cache.c']
    name = src.split('.')[0]
    test_exe = executable(name, src,
        c_args: test_c_args,
        dependencies: test_comp_common_dep,
    )
    test(name, test_exe)
endforeach

# Tests needing idk_fs_lib (public API)
foreach src : ['test_idk_fs.c']
    name = src.split('.')[0]
    test_exe = executable(name, src,
        c_args: test_c_args,
        include_directories: inc,
        link_with: [idk_fs_lib],
    )
    test(name, test_exe)
endforeach

# Static library for ipc.c - validation tests
test_ipc_lib = static_library('test_ipc',
    '../src/ipc/ipc.c',
    c_args: test_c_args,
    include_directories: inc,
)

test_exe = executable('test_ipc', 'test_ipc.c',
    c_args: test_c_args,
    include_directories: inc,
    link_with: [test_ipc_lib],
)
test('test_ipc', test_exe)
