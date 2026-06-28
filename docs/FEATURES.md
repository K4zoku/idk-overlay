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
| Backend env var | `IDK_TP_BACKEND=socket` | `IDK_TP_BACKEND=shm` |

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
| LD_PRELOAD | 100% | ✓ bypass | ⚠️ helper OOM | default path, via launcher |
| kernel module | planned | ✓ bypass | ✓ | nuclear option |

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
