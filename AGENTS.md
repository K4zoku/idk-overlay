# idk-overlay

## Build & Test
```bash
meson setup build -Dspirv=true        # SPIR-V optional (needs glslc)
meson compile -C build -j$(nproc)
meson test -C build
```

Tests are standalone C executables with a custom `TEST()`/`RUN()`/`ASSERT_*` macro framework (`tests/test_runner.h`). No test framework binary.

## Codebase Structure
- `src/hook/overlay.c` — orchestrator, plugin discovery, lib filtering via `dlopen(RTLD_NOLOAD)` (not process-name)
- `src/hook/egl_hook.c`, `glx_hook.c`, `vulkan_hook.c`, `vulkan_layer.c` — graphics backend plugins
- `src/core/compositor_*.c` — compositor engine (socket, ACK, resize, DMABUF/SHM)
- `include/public/idk_ipc.h` — wire protocol types (28B frame, 16B ACK, 20B input event)
- `include/public/idk_fs.h` — frame sender public API (used by webview)
- `subprojects/syringe.wrap` — syringe injection toolkit (meson wrap-git from github.com/K4zoku/syringe)

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
