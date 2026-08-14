# idk-overlay

## Build & Test
```bash
meson setup build                    # SPIR-V optional (auto-detected via glslc)
meson compile -C build -j$(nproc)
meson test -C build
```

The CEF webview (`idk-webview-cef`) is optional — built only when the CEF dist
is provided:
```bash
meson setup build -Dcef_dist=/path/to/cef_binary_<ver>_linux64
```

Tests are standalone C executables with a custom `TEST()`/`RUN()`/`ASSERT_*` macro framework (`tests/test_runner.h`). No test framework binary.

## Codebase Structure
- `src/hook/preload/` — LD_PRELOAD entry: `on_load()` constructor, target-process filter, GL/VK lib polling, webview fork + monitor, hotkey config
  - `ctor.c` `on_load()`: checks `idk_is_target_process()`, spawns `hook_install_thread`
  - `install.c` `hook_install_thread`: polls for GL/VK libs (phase 1), broker decision (phase 2 — `detect_wine()` + `connect_via_broker()`, signals `g_broker_cond`), forks webview or uses broker, installs plugins (phase 3)
- `src/hook/gl/` — GLX/EGL symbol interception (`glx.c`, `egl.c` — `idk_hook_plugin_t` plugins) + `dlsym.c` (dlsym interposer)
- `src/hook/vklayer/` — Vulkan layer (no constructor; lazy init from `vkCreateInstance`): negotiation, GPA dispatch, create-device ext injection, present hook
- `src/hook/input/` — input capture backends (`wayland/`, `x11/`), shared input socket sender
- `src/core/compositor/` — shared compositor singleton (`g_comp`), frame recv/ack/request, resize debounce, SHM cache, path helpers, broker state sync (`g_broker_state` + `pthread_cond_t`)
- `src/core/transport/` — wire transport: `dispatch.c` (backend vtable), `socket/` (AF_UNIX), `shm/` (shared memory, futex/pidfd)
- `src/core/render/` — render backends (`gl/`, `vk/`): composite the overlay frame into the game frame
- `src/producer/` — `idk_producer_*` client API (frame sender, used by webview)
- `src/cli/broker/` — host-ns webview spawner (`main.c` accept loop, `session.c`, `spawn.c`, `reap.c`)
- `src/cli/webview_cef/` — CEF (Chromium) webview backend: windowless OSR + shared-texture dmabuf + input thread (C++20, needs `-Dcef_dist=`) — `main.cc` (CEF lifecycle), `view.cc` (render handler + frame send + REQUEST/ACK pacing), `input.cc` (input socket → CEF events), `config.cc` (INI), `keys.cc` (keysym → VK)
- `include/core/compositor.h` — `idk_compositor_t` struct + all pure helper declarations + API
- `include/public/idk_ipc.h` — wire protocol umbrella (`idk_cp.h`, `idk_frames.h`, `idk_input.h`)
- `include/public/idk_producer.h` — frame producer public API (used by webview)
- `subprojects/syringe.wrap` — syringe injection toolkit (meson wrap-git from github.com/K4zoku/syringe)

## Broker State Sync
The broker decision (`g_broker_state`) is a 3-way synchronization point:
1. **`hook_install_thread`** phase 2 (src/hook/preload/install.c) runs `detect_wine()` + `connect_via_broker()`, sets state and signals via `pthread_cond_signal`
2. **`idk_compositor_init`** (called from backend hooks) waits via `pthread_cond_timedwait` (5s timeout), picks up `IDK_TP_ABSTRACT` env var set by broker
3. **`hook_install_thread`** re-reads the state after the wait to pick fork-webview vs broker mode

State values: 0=pending, 1=broker-connected, 2=no-broker, 3=broker-failed
Default is 2 (no broker) so test static lib and external callers never block.
`on_load()` resets to 0 before spawning threads.

## Env Vars
- `IDK_TP_ABSTRACT` — abstract socket name for transport (set by broker in `connect_via_broker()`)
- `IDK_INPUT_ABSTRACT` — abstract socket name for input (set together with transport)
- `IDK_BROKER=1` — force broker mode (wine detection is automatic otherwise)
- `IDK_SOCKET` — override filesystem socket path (ignored when `IDK_TP_ABSTRACT` is set)
- `IDK_WEBVIEW` — webview binary name to spawn (default `idk-webview`; `idk-webview-cef` selects the CEF backend). Honored by both the hook fork path (`src/hook/preload/fork.c`) and the broker (`src/cli/broker/spawn.c`). `IDK_WEBVIEW_BIN` (absolute path) still wins.
- `IDK_CEF_DIR` — CEF dist root for `idk-webview-cef` (default: compiled-in `-Dcef_dist` path)
- `IDK_CEF_DEBUG_PORT` — `idk-webview-cef`: enable DevTools on this port (optional)

## Broker Mode Flow
1. Overlay (inside Wine ns) → connects to broker on abstract `\0idk_broker_<uid>`
2. Handshake sends `idk_cp_handshake_t` with transport/input abstract names
3. Broker (host ns) → `fork/exec` idk-webview → sets `IDK_TP_ABSTRACT` + `IDK_INPUT_ABSTRACT` in webview env
4. Webview connects directly to overlay's transport/input sockets — broker out of hot path
5. Game exit → overlay control socket EOF → broker kills webview

## Conventions
- **Language**: C11 (`gnu11`), with a small C++17 Qt6 webview in `src/cli/webview/` and a C++20 CEF webview in `src/cli/webview_cef/` (CEF headers require C++20)
- **Code style**: LLVM clang-format at 120 columns, 4-space indent, LF, trailing whitespace trimmed
- **No unnecessary comments**: do not add comments that restate what the code already says. Do not add blank-line separators within functions. Let the code speak.
- **No backward compat**: don't preserve deprecated APIs, shims, or compatibility layers unless explicitly asked.
- **Performance over complexity**: prefer the faster path even if it makes code less elegant. This is a real-time overlay composited into a game's frame — every microsecond matters.
- **No `-fstack-protector`** (inject library targets) — game injection requires it off
- **Wire protocol**: all structs have `_Static_assert` on `sizeof()` and `#pragma pack(push, 1)` — always update both when changing types
- **Shaders**: GLSL embedded via `ld -r -b binary` + `objcopy` symbol rename; SPIR-V optional via `glslc` (auto-detected)

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

## CEF Webview
Only built with `-Dcef_dist=<dist>` (optional). Windowless OSR with shared
textures: frames arrive as multi-plane dmabufs in `OnAcceleratedPaint`
(verified on i915/Wayland) and are sent via `idk_producer_send_dma_buf`;
rejected dmabufs fall back to SHM (linear only — tiled needs a GPU readback,
not implemented). Notes:
- `icudtl.dat` must sit next to `libcef.so` (`DIR_ASSETS`); the binary copies
  it there once from `Resources/` at startup.
- The dist's ANGLE `libEGL.so.1` shadows system EGL in the same process — do
  not call EGL symbols directly.
- Teardown is process exit (`_exit(0)`) — `CefShutdown` trips CEF's
  shutdown-checker with the still-referenced View.
- `tests/test_webview_cef.c` spawns the binary against a fake compositor:
  dmabuf flow + SHM fallback + shutdown (needs a display).
