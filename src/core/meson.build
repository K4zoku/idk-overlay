
linux_sources = files('linux/fd.c', 'linux/sync.c')

subdir('compositor')
subdir('transport')
subdir('render')

core_sources = files('log.c', 'broker.c', 'diagnostics.c') + linux_sources + compositor_sources + transport_sources
