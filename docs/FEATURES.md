# Feature Matrix

## Transport Backends

| Feature | Socket | SHM+futex |
|---------|--------|-----------|
| Frame delivery | ✓ | ✓ |
| Input delivery | ✓ | planned |
| Duplex (ACK/REQUEST) | ✓ | ✓ |
| Simplex mode | planned | planned |
| Cross-process | ✓ | ✓ (fork only) |
| Reconnect | ✓ | ✓ |
| Abstract namespace (broker mode) | ✓ | — (pidfd / `tp_fdchan` planned) |
| Backend env var | `IDK_TP_BACKEND=socket` | `IDK_TP_BACKEND=shm` |

In broker mode (Wine mount namespace) the transport is socket-only —
the SHM + `tp_fdchan` plan is tracked under "Deferred to v2" in
`specs.md`. Abstract sockets (`sun_path[0] == '\0'`) are selected
conventionally by `tp_socket` whenever the name passed in starts with a
NUL byte, so `idk_tp_init` / `idk_fs_init` accept abstract names
without a separate `_abstract` entry point.

## Graphics Backends

| Feature | EGL | GLX | Vulkan |
|---------|-----|-----|--------|
| Overlay render | ✓ | ✓ | ✓ |
| DMABUF export | ✓ | ✓ (GL_EXT_memory_object) | ✓ |
| SHM fallback | ✓ | ✓ | ✓ |
| Cross-GPU fallback | ✓ | ✓ | ✓ |
| Resize detection | eglQuerySurface | glXQueryDrawable | vkCreateSwapchainKHR |
| Resize debounce | ✓ compositor-side | ✓ compositor-side | ✓ compositor-side |
| Hook method | GOT patch | GOT patch | Vulkan layer |
| Skip if other active | — | skips if EGL active | — |

## Input Backends

| Feature | Wayland | X11 |
|---------|---------|-----|
| Keyboard swallow | ✓ | ✓ |
| Mouse swallow | ✓ | ✓ |
| Scroll/axis | ✓ | ✓ |
| Key repeat | rate/delay from compositor | X11 synthetic pair detection |
| Retroactive patch | ✓ proxy_list scan | ✓ XSelectInput rescan |
| Cursor toggle | ✓ | N/A |
| Hook method | wl_proxy implementation overwrite | XNextEvent + 7 variants |
| Priority | fallback if X11 detected | preferred for XWayland |

## Injection Methods

| Method | Success rate | Anti-debug | Wine/Proton | Notes |
|--------|-------------|------------|-------------|-------|
| ptrace (syringe) | high | ✗ blocked | ✗ blocked | default live injection path |
| dotnet-diagnostic | 100% | ✓ bypass | ✗ N/A | dotnet games only |
| LD_PRELOAD | 100% | ✓ bypass | ⚠️ helper OOM; ✓ via broker | broker (`idk-broker`) hosting the webview | default path, via launcher |
| kernel module | planned | ✓ bypass | ✓ | nuclear option |

## Broker Mode (Wine mount namespace)

| Feature | Status |
|---------|--------|
| Per-user abstract socket `\0idk_broker_<uid>` | ✓ |
| `SO_PEERCRED` same-uid auth on accept | ✓ |
| Handshake `{ identity, comm, tp_socket, input_socket, tp_backend }` | ✓ (`idk_cp_handshake_t`, 204 bytes packed) |
| Host-namespace webview exec | ✓ (`idk-broker` → `idk-webview`) |
| Overlay auto-detect (`.exe` / `/proc/self/maps` scan) | ✓ |
| Force via `IDK_BROKER=1` | ✓ |
| Wine auto-detect, no broker → `fork_webview()` fallback | ✓ |
| Forced + broker unreachable → overlay disabled | ✓ |
| Game-exit → broker kills webview (EOF on control sock) | ✓ |
| Crash restart (max 3×) | planned (v2) |
| SHM backend in broker mode | planned (v2, needs cross-ns `tp_fdchan`) |
| `IDK_BROKER_FD` structured crash reason | planned (v2) |

## Webview Backends

| Feature | Qt (QtWebEngine) | CEF (planned) |
|---------|-----------------|---------------|
| Frame export | QRhi → EGL/Vulkan DMABUF | OnAcceleratedPaint → ANGLE/EGL |
| SHM fallback | ✓ glReadPixels | ✓ OnPaint CPU buffer |
| Input inject | QKeyEvent/QMouseEvent via focusProxy | SendKeyEvent/SendMouseClickEvent |
| Lazy rendering | ✓ REQUEST-driven fp->update() | ✓ REQUEST-driven |
| Key repeat | QTimer rate/delay | built-in |
| IME | planned zwp_text_input_v3 | planned |
| Offscreen FPS cap | none (REQUEST-driven) | SetWindowlessFrameRate() |
