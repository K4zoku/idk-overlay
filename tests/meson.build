test_c_args = [
    '-D_GNU_SOURCE',
    '-Wall',
    '-Wextra',
    '-O2',
    '-Wno-pedantic',
]

# Static library of compositor functions + transport (no GPU deps)
test_comp_lib = static_library('test_comp',
    ['../src/core/log.c'] + compositor_sources + transport_sources,
    c_args: test_c_args,
    include_directories: inc,
)

test_comp_dep = declare_dependency(
    link_with: test_comp_lib,
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

# Tests needing compositor/transport symbols
foreach src : ['test_vendor.c', 'test_resize.c', 'test_shm_cache.c',
               'test_abstract_socket.c', 'test_compositor_path.c',
               'test_compositor_api.c']
    name = src.split('.')[0]
    test_exe = executable(name, src,
        c_args: test_c_args,
        dependencies: test_comp_dep,
    )
    test(name, test_exe)
endforeach

# Tests needing idk_producer_lib (public API)
foreach src : ['test_idk_producer.c']
    name = src.split('.')[0]
    test_exe = executable(name, src,
        c_args: test_c_args,
        include_directories: inc,
        link_with: [idk_producer_lib],
    )
    test(name, test_exe)
endforeach

# End-to-end CEF webview test (built only with -Dcef_dist)
cef_dist = get_option('cef_dist')
if cef_dist != ''
    test_cef_exe = executable('test_webview_cef', 'test_webview_cef.c',
        c_args: test_c_args,
        include_directories: inc,
        dependencies: test_comp_dep,
    )
    test('test_webview_cef', test_cef_exe,
        args: [idk_webview_cef_exe],
        is_parallel: false,
        timeout: 60,
    )
endif

# Static library for ipc.c - validation tests
test_ipc_lib = static_library('test_ipc',
    '../src/ipc/ipc.c',
    '../src/core/log.c',
    c_args: test_c_args,
    include_directories: inc,
)

test_exe = executable('test_ipc', 'test_ipc.c',
    c_args: test_c_args,
    include_directories: inc,
    link_with: [test_ipc_lib],
)
test('test_ipc', test_exe)
