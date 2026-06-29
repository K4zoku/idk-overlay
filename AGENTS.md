# idk-overlay

## Build & Test
```bash
meson setup build -Dspirv=true        # SPIR-V optional (needs glslc)
meson compile -C build -j$(nproc)
meson test -C build
```

Tests are standalone C executables with a custom `TEST()`/`RUN()`/`ASSERT_*` macro framework (`tests/test_runner.h`). No test framework binary.

## Codebase Structure
- `src/hook/overlay.c` — orchestrator, constructor, plugin discovery, lib filtering via `dlopen(RTLD_NOLOAD)`
  - `on_load()` constructor: checks `idk_is_target_process()`, spawns `broker_connect_thread` + `hook_install_thread`
  - `broker_connect_thread`: runs `detect_wine()` + broker connect concurrently, signals via `g_broker_cond`
  - `hook_install_thread`: polls for GL/VK libs (Phase 1), waits for broker decision (Phase 2), forks webview or uses broker, installs hooks (Phase 3)
- `src/hook/egl_hook.c`, `glx_hook.c`, `vulkan_hook.c`, `vulkan_layer.c` — graphics backend plugins (call `idk_compositor_init/recv_frame/send_ack`)
- `src/core/compositor.c` — shared compositor singleton (`g_comp`), transport init/recv/send/resize/SHM-cache/vendor-detect, broker state sync (`g_broker_state` + `pthread_cond_t`)
- `src/core/transport.c`, `tp_socket.c`, `tp_shm.c` — wire transport layers
- `src/cli/broker/main.c` — host-ns webview spawner (spawns detached thread per session)
- `include/core/compositor.h` — `idk_compositor_t` struct + all pure helper declarations + API
- `include/public/idk_ipc.h` — wire protocol types (28B frame, 16B ACK, 20B input event)
- `include/public/idk_fs.h` — frame sender public API (used by webview)
- `subprojects/syringe.wrap` — syringe injection toolkit (meson wrap-git from github.com/K4zoku/syringe)

## Broker State Sync
The broker decision (`g_broker_state`) is a 3-way synchronization point:
1. **`broker_connect_thread`** sets state and signals via `pthread_cond_signal`
2. **`idk_compositor_init`** (called from backend hooks) waits via `pthread_cond_timedwait` (5s timeout), picks up `IDK_TP_ABSTRACT` env var set by broker
3. **`hook_install_thread`** Phase 2 polls `g_broker_state` every 50ms (5s total) — forks webview or uses broker

State values: 0=pending, 1=broker-connected, 2=no-broker, 3=broker-failed
Default is 2 (no broker) so test static lib and external callers never block.
`on_load()` resets to 0 before spawning threads.

## Env Vars
- `IDK_TP_ABSTRACT` — abstract socket name for transport (set by broker in `connect_via_broker()`)
- `IDK_INPUT_ABSTRACT` — abstract socket name for input (set together with transport)
- `IDK_BROKER=1` — force broker mode (wine detection is automatic otherwise)
- `IDK_SOCKET` — override filesystem socket path (ignored when `IDK_TP_ABSTRACT` is set)

## Broker Mode Flow
1. Overlay (inside Wine ns) → connects to broker on abstract `\0idk_broker_<uid>`
2. Handshake sends `idk_cp_handshake_t` with transport/input abstract names
3. Broker (host ns) → `fork/exec` idk-webview → sets `IDK_TP_ABSTRACT` + `IDK_INPUT_ABSTRACT` in webview env
4. Webview connects directly to overlay's transport/input sockets — broker out of hot path
5. Game exit → overlay control socket EOF → broker kills webview

## Conventions
- **Language**: C11 (`gnu11`), with a small C++17 Qt6 webview in `src/cli/webview/`
- **Code style**: LLVM clang-format at 120 columns, 4-space indent, LF, trailing whitespace trimmed
- **No unnecessary comments**: do not add comments that restate what the code already says. Do not add blank-line separators within functions. Let the code speak.
- **No backward compat**: don't preserve deprecated APIs, shims, or compatibility layers unless explicitly asked.
- **Performance over complexity**: prefer the faster path even if it makes code less elegant. This is a real-time overlay composited into a game's frame — every microsecond matters.
- **No `-fstack-protector`** (inject library targets) — game injection requires it off
- **Wire protocol**: all structs have `_Static_assert` on `sizeof()` and `#pragma pack(push, 1)` — always update both when changing types
- **Shaders**: GLSL embedded via `ld -r -b binary` + `objcopy` symbol rename; SPIR-V optional via `glslc` (toggle `-Dspirv=true`)

## Vulkan Layer
The layer manifest is generated from `src/hook/idk_overlay.json.in`. Enable at runtime:
```sh
VK_LAYER_PATH=build IDK_VK_LAYER=1 VK_INSTANCE_LAYERS=VK_LAYER_IDK_overlay <game>
```
Disabled by `IDK_VK_DISABLE=1`.

## Packaging
Arch Linux PKGBUILD at `dist/PKGBUILD` — uses `arch-meson --wrap-mode=default`.

## Qt6 Webview
Only built when Qt6 is found on the system (optional dep). Uses `qt.preprocess(moc_headers: ...)` for MOC — add new QObject headers to `webview_moc_headers` in `src/cli/webview/meson.build`.
