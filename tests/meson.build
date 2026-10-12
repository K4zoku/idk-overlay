test_c_args = [
    '-D_GNU_SOURCE',
    '-Wall',
    '-Wextra',
    '-O2',
    '-Wno-pedantic',
]

# Static library of compositor functions + transport (no GPU deps)
test_comp_lib = static_library('test_comp',
    ['../src/core/log.c'] + linux_sources + compositor_sources + transport_sources,
    c_args: test_c_args,
    include_directories: inc,
)

test_comp_dep = declare_dependency(
    link_with: test_comp_lib,
    include_directories: inc,
)

# Header-only tests (no linking needed)
foreach src : ['test_wire_protocol.c', 'test_input.c', 'test_ack.c', 'test_shm_delta.c']
    name = src.split('.')[0]
    test_exe = executable(name, src,
        c_args: test_c_args,
        include_directories: inc,
    )
    test(name, test_exe)
endforeach

# X11 hook contract tests with a fake XSelectInput implementation.
test_x11_select_exe = executable('test_x11_select',
    ['test_x11_select.c', '../src/hook/input/x11/select.c'],
    c_args: test_c_args,
    include_directories: inc,
)
test('test_x11_select', test_x11_select_exe)

# Tests needing compositor/transport symbols
foreach src : ['test_vendor.c', 'test_resize.c', 'test_shm_cache.c',
               'test_abstract_socket.c', 'test_compositor_path.c',
               'test_compositor_api.c', 'test_cursor_transport.c',
               'test_fd_leaks.c', 'test_socket_controls.c', 'test_hidden_discard.c',
               'test_linux_fd.c', 'test_linux_sync.c', 'test_socket_v2.c']
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

# CEF dirty-rect geometry (standalone; no CEF binary or GPU needed).
cef_damage_test = executable('test_cef_damage', 'test_cef_damage.cc', cpp_args: ['-std=c++20'])
test('test_cef_damage', cef_damage_test)

cef_args_test = executable('test_webview_cef_args',
    ['test_webview_cef_args.cc', '../src/cli/webview_cef/config.cc', '../src/core/log.c'],
    c_args: test_c_args,
    cpp_args: ['-std=c++20'],
    include_directories: inc,
)
test('test_webview_cef_args', cef_args_test)

# CEF BGRA/top-left -> GL SHM RGBA/bottom-left normalization (no CEF distro required).
cef_shm_format_test = executable('test_cef_shm_format', 'test_cef_shm_format.cc',
    cpp_args: ['-std=c++20'], include_directories: inc)
test('test_cef_shm_format', cef_shm_format_test)

cef_shm_diff_test = executable('test_cef_shm_diff', 'test_cef_shm_diff.cc',
                                include_directories: inc, cpp_args: ['-std=c++20'])
test('test_cef_shm_diff', cef_shm_diff_test)
