project('idk-client', 'c', version: '0.1.0')

# ── libidk-client ────────────────────────────────────────────────────────
# Shared library for sending overlay frames to idk-overlay socket
#
# Provides the idk_client_* API used by:
#   - idk-client binary (standalone frame sender)
#   - External processes (webview, Discord, etc.)

client_c_flags = [
    '-D_GNU_SOURCE',
    '-Wall',
    '-Wextra',
    '-O2',
]

idk_client_lib = shared_library(
    'idk-client',
    'src/idk_client.c',
    c_args: client_c_flags,
    include_directories: include_directories('include'),
    link_args: ['-ldl', '-lpthread'],
    version: '0.1',
    install: true,
)

idk_client_dep = declare_dependency(
    include_directories: include_directories('include'),
    link_with: idk_client_lib,
)

message('  libidk-client:    shared library (frame sender)')
