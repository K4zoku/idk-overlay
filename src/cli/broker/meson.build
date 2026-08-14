# idk-broker — host-namespace webview spawner (Wine/Proton)
idk_broker_exe = executable(
    'idk-broker',
    files('main.c', 'net.c', 'handshake.c', 'spawn.c', 'reap.c', 'session.c'),
    c_args: overlay_c_flags,
    include_directories: inc,
    install: true,
)