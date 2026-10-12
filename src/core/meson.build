
linux_sources = files('linux/fd.c')

subdir('compositor')
subdir('transport')
subdir('render')

core_sources = files('log.c', 'broker.c') + linux_sources + compositor_sources + transport_sources
