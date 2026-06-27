# idk-overlay

**Wayland-native game overlay platform for Linux.**

Injects into a target process via [syringe](https://github.com/K4zoku/syringe) (ptrace / LD_PRELOAD / .NET diagnostic IPC), hooks graphics present functions, and composites a webview-rendered overlay on top of the game's framebuffer.

## Features

- **3 injection paths**: LD_PRELOAD (universal), ptrace (live process), .NET diagnostic IPC (bypasses anti-debug)
- **Wayland-native**: works on Wayland without X11 fallback
- **Qt6 WebEngine webview**: render HTML/CSS/JS overlays
- **DMABUF import**: zero-copy texture import via EGL (`EGL_EXT_image_dma_buf`) or `GL_EXT_memory_object` (GLX fallback)
- **Wayland input hooking**: toggle input capture (default: Shift+Tab) to redirect keyboard + mouse to the overlay UI
- **Plugin architecture**: per-API hooks register via `idk_hook_plugin_t`, auto-discovered
- **Double-buffered GL textures**: no flickering, no tearing
- **ACK flow control**: webview frame rate synced to game swap rate
- **GL state save/restore**: comprehensive, based on MangoHud approach
- **Program auto-recovery**: detects GL driver reusing program IDs, re-inits shaders
- **SPIR-V shaders**: preferred when available, GLSL fallback

## Architecture

```
┌──────────────────────┐                ┌──────────────────────┐
│  Webview Client      │  Unix socket   │  Target Process      │
│  (idk-webview, Qt6)  │  SCM_RIGHTS    │  (game)              │
│                      │   fd + meta    │                      │
│  Render HTML page    │  ──────────►   │  Injection via:      │
│  Export as SHM/DMABUF│  ◄──────────   │  1. LD_PRELOAD       │
│  (memfd or dma-buf)  │   ACK + resize │  2. ptrace (syringe) │
│                      │                │  3. .NET diagnostic  │
│  Wait for ACK        │                │                      │
│  before next paint   │                │  Plugin: EGL/GLX/VK  │
│                      │                │  → recv overlay      │
│                      │                │  → import SHM/DMABUF │
│                      │                │  → render quad       │
│                      │                │  → call orig swap    │
└──────────────────────┘                └──────────────────────┘
```

## Components

| Component | Description |
|-----------|-------------|
| `libidk-overlay.so` | Injectable library — hooks EGL/GLX/Vulkan, composites overlay |
| `libidk-framesource.so` | Frame sender library (used by webview) |
| `idk-webview` | Qt6 WebEngine webview client |
| `idk-inject` | CLI — injects libidk-overlay.so into running process |
| `bin/idk-overlay` | Wrapper script — launches game with overlay |

## Build

```bash
# Dependencies (Arch):
sudo pacman -S meson ninja gcc pkg-config libglvnd libegl mesa vulkan-icd-loader
# Optional: qt6-base qt6-webengine glmark2

# Build
meson setup build
ninja -C build

# Install (optional)
sudo meson install -C build
```

## Usage

### Option 1: LD_PRELOAD (recommended)

```bash
# Launch game with overlay + auto-start webview
./bin/idk-overlay --qt-client osu-lazer

# Or manually (socket defaults to $XDG_RUNTIME_DIR/idk-overlay-<pid>,
# /tmp/idk-overlay-<pid> fallback if XDG_RUNTIME_DIR is unset):
LD_PRELOAD=./build/libidk-overlay.so osu-lazer &
./build/src/cli/webview/idk-webview
```

### Option 2: ptrace inject (live process)

```bash
# Start game first, then inject
osu-lazer &
./build/src/cli/inject/idk-inject $(pgrep osu!) -v
# → Auto-detects .NET → diagnostic IPC (no ptrace, no root)
# → Or ptrace for native apps

# Start webview (socket path defaults to $XDG_RUNTIME_DIR/idk-overlay-<pid>)
./build/src/cli/webview/idk-webview
```

### Environment variables

| Variable | Default | Description |
|----------|---------|-------------|
| `IDK_SOCKET` | `$XDG_RUNTIME_DIR/idk-overlay-<pid>` (or `/tmp/idk-overlay-<pid>` fallback) | Unix socket path (PID-based, avoids conflicts) |
| `IDK_VK` | `1` | Enable Vulkan hooks |
| `IDK_GL` | `1` | Enable OpenGL/EGL hooks |
| `IDK_DEBUG` | (unset) | Set to `1` to enable debug logging |
| `IDK_HOTKEY_CAPTURE` | `Shift+Tab` | Hotkey to toggle input capture |
| `IDK_HOTKEY_OVERLAY` | `F8` | Hotkey to toggle overlay visibility |

## Wayland input hooking

The overlay can capture keyboard and mouse input when toggled by a hotkey (default: **F8**).

1. Hooks `wl_proxy_add_listener` in `libwayland-client.so.0`
2. Substitutes a wrapper vtable that saves the game's listener + user_data
3. Forwards events to the game by default; toggling capture swallows them and forwards to the webview via `${IDK_SOCKET}-input`
4. Hotkey again releases capture; events resume flowing to the game

### Input IPC protocol

Separate socket from the frame socket (no `SCM_RIGHTS`). Path: `${IDK_SOCKET}-input`.

Each message is 20 bytes (`idk_input_event_t`):

```
+----------------------------------------------------+
| type   uint8   |  KEY/BUTTON/MOTION/AXIS/STATE     |
| flags  uint8   |  bit0=press, bit1=capture         |
| mods   uint16  |  Ctrl=1, Shift=2, Alt=4, Super=8  |
| time   uint32  |  Wayland timestamp (ms)           |
| union  (12B)   |  key { keycode, keysym, _pad }    |
|                |  btn { x, y, button }             |
|                |  motion { x, y, _pad }            |
|                |  axis { dx, dy, _pad }            |
|                |  repeat { rate, delay, _pad }     |
+----------------------------------------------------+
```

## Project structure

```
src/
├── core/               Compositor engine
│   ├── compositor_common.c  Shared: socket init, ACK, resize debounce
│   ├── compositor_egl.c     EGL+GL compositor (DMABUF via EGL_EXT_image_dma_buf)
│   └── compositor_vk.c      Vulkan compositor (DMABUF via VK_EXT_external_memory)
├── gl/                 GL function loader + shader compiler
│   ├── gl_loader.c          GL function pointer resolution at runtime
│   └── shader_loader.c      Shader compile (SPIR-V + GLSL fallback)
├── hook/               Graphics + input hooks
│   ├── overlay.c            Orchestrator — background polling, plugin discovery
│   ├── egl_hook.c           EGL swap hook plugin
│   ├── glx_hook.c           GLX swap hook plugin
│   ├── vulkan_hook.c        Vulkan syringe hook plugin
│   ├── vulkan_layer.c       Vulkan layer (VK_LAYER_PATH)
│   └── wayland/             Wayland input capture (6 files)
├── ipc/                Wire protocol (frame header 28B, input event 20B)
├── lib/                libidk-framesource.so (frame sender for webview)
├── shaders/            GLSL + Vulkan SPIR-V shaders
└── cli/
    ├── inject/         idk-inject CLI tool (wraps syringe_inject)
    └── webview/        Qt6 WebEngine client
        ├── main.cpp, manager.cpp
        ├── view/       webview.cpp, rhi_texture_extractor.cpp
        ├── input/      input_receiver.cpp
        └── config/     groupconfig.cpp

include/
├── core/               Compositor + log headers
├── gl/                 GL loader + shader headers
├── hook/               Plugin interface, hook utilities, wayland input
├── public/             Public API (idk_fs.h, idk_ipc.h)
├── shaders/            VK shader symbols (SPIR-V embedded)
└── webview/            Webview private headers
```

## Plugin interface

Each graphics backend registers via `idk_hook_plugin_t`:

```c
typedef struct idk_hook_plugin {
    const char *name;
    const char *lib_patterns[4];  // .so names to probe
    int  (*init)(void);
    void (*shutdown)(void);
} idk_hook_plugin_t;
```

To add a new hook: implement the plugin, add `&idk_plugin_foo` to `g_plugins[]` in `overlay.c`.

## Injection path detection

The library auto-detects whether it was loaded via LD_PRELOAD or late-injected:

- **LD_PRELOAD**: the dynamic linker has already resolved the hooked symbol to our address (`RTLD_DEFAULT` matches our function pointer) — no syringe GOT patching needed.
- **Late inject**: the real function is already resolved; syringe patches GOT/PLT to redirect calls to our implementation.

Detection is a single `dlsym(RTLD_DEFAULT, sym) == our_func` check in `hook/hook_util.h`.

## Tested on

| Target | LD_PRELOAD | ptrace | .NET IPC |
|--------|------------|--------|----------|
| glmark2-egl | ✅ | ✅ | N/A |
| osu! lazer (.NET) | ✅ | ❌ (anti-debug) | ✅ |
| eglgears_wayland | ✅ | ✅ | N/A |

## Dependencies

- [syringe](https://github.com/K4zoku/syringe) — injection toolkit (subproject)
- Linux x86-64 (aarch64 code written, untested)
- EGL + OpenGL or Vulkan
- Qt6 WebEngine (for webview client, optional)

## License

MIT
