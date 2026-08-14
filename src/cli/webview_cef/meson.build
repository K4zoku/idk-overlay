# idk-webview-cef — CEF (Chromium) webview backend: OSR windowless + dmabuf +
# input. Built only when -Dcef_dist=<path to CEF linux64 dist> is given.
cef_dist = get_option('cef_dist')

if cef_dist != ''
  cef_inc = include_directories(cef_dist)

  # libcef_dll_wrapper sources from the dist (C++20 required by CEF headers).
  wrapper_find = run_command(
      'sh', '-c',
      'cd "$1" && find libcef_dll -name "*.cc" -not -path "*/test/*" | sort',
      'cef', cef_dist,
      check: true,
  )
  wrapper_src = []
  foreach f : wrapper_find.stdout().strip().split('\n')
    if f != ''
      wrapper_src += files(cef_dist / f)
    endif
  endforeach

  cef_wrapper_lib = static_library(
      'cef_dll_wrapper',
      wrapper_src,
      include_directories: cef_inc,
      cpp_args: [
          '-std=c++20',
          '-DWRAPPING_CEF_SHARED',
          '-fno-exceptions',
          '-fno-rtti',
          '-O2',
          '-w',
      ],
  )

  cef_webview_inc = include_directories('../../../include')
  idk_webview_cef_exe = executable(
      'idk-webview-cef',
      files('app.h', 'config.cc', 'input.cc', 'keys.cc', 'main.cc', 'view.cc'),
      include_directories: [inc, cef_inc],
      dependencies: [idk_producer_dep, dl_lib, pthread_lib],
      cpp_args: [
          '-std=c++20',
          '-DWRAPPING_CEF_SHARED',
          '-DCEF_DIST_DIR="' + cef_dist + '"',
      ],
      link_with: [cef_wrapper_lib],
      link_args: [
          '-L' + cef_dist / 'Release',
          '-lcef',
          '-Wl,-rpath,' + cef_dist / 'Release',
      ],
      install: true,
  )
endif
