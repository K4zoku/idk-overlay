# idk-overlay

**Open-source, Wayland-native game overlay platform for Linux.**

Injects into a target process via [syringe](https://github.com/K4zoku/syringe) (ptrace / LD_PRELOAD / .NET diagnostic IPC), hooks graphics present functions, and composites a webview-rendered overlay on top of the game's framebuffer.

## Features

- **3 injection paths**: LD_PRELOAD (universal), ptrace (live process), .NET diagnostic IPC (bypasses anti-debug)
- **Wayland-native**: works on Wayland without X11 fallback
- **Live process injection**: inject into already-running games (not just LD_PRELOAD)
- **.NET support**: injects into osu! lazer via diagnostic IPC — no ptrace, no root, no anti-debug bypass
- **Qt6 WebEngine webview**: render HTML/CSS/JS overlays
- **Wayland input hooking**: toggle input capture (default: F8) to redirect keyboard + mouse to the overlay UI
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
│  Export as SHM       │  ◄──────────   │  1. LD_PRELOAD       │
│  (memfd)             │   ACK byte     │  2. ptrace (syringe) │
│                      │                │  3. .NET diagnostic  │
│  Wait for ACK        │                │                      │
│  before next paint   │                │  Hooks eglSwapBuffers│
│                      │                │  → import SHM frame  │
│                      │                │  → upload to texture │
│                      │                │  → render fullscreen │
│                      │                │    quad on top       │
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
| `idk-render` | Standalone render process (optional, for frame capture) |
| `idk-send-frame` | CLI — sends raw frames from external process |
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

# Or manually:
IDK_SOCKET=/tmp/idk-overlay-$$ LD_PRELOAD=./build/libidk-overlay.so osu-lazer &
IDK_SOCKET=/tmp/idk-overlay-$$ ./build/src/cli/webview/idk-webview
```

### Option 2: ptrace inject (live process)

```bash
# Start game first, then inject
osu-lazer &
./build/src/cli/inject/idk-inject $(pgrep osu!) -v
# → Auto-detects .NET → diagnostic IPC (no ptrace, no root)
# → Or ptrace for native apps

# Start webview
IDK_SOCKET=/tmp/idk-overlay-$(pgrep osu!) ./build/src/cli/webview/idk-webview
```

### Environment variables

| Variable | Default | Description |
|----------|---------|-------------|
| `IDK_SOCKET` | `/tmp/idk-overlay-<pid>` | Unix socket path (PID-based, avoids conflicts) |
| `IDK_VK` | `1` | Enable Vulkan hooks |
| `IDK_GL` | `1` | Enable OpenGL/EGL hooks |
| `IDK_DEBUG` | (unset) | Set to `1` to enable debug logging |
| `IDK_TOGGLE_KEY` | `F8` | Hotkey to toggle input capture (xkb keysym name: `F1`–`F12`, `Scroll_Lock`, `Pause`, etc.) |

## Wayland input hooking

The overlay can capture keyboard and mouse input when toggled by a hotkey (default: **F8**). This lets the overlay UI receive text input, clicks, etc. without the game seeing them.

### How it works

1. `libidk-overlay.so` hooks `wl_proxy_add_listener` (exported by `libwayland-client.so.0`).
2. When the game registers a `wl_pointer` or `wl_keyboard` listener, the hook substitutes a wrapper vtable that saves the game's listener + user_data.
3. The wrapper forwards events to the game by default. When the user presses the hotkey, capture mode toggles on.
4. While captured, the wrapper swallows events from the game and forwards them to the webview via a separate Unix socket (`${IDK_SOCKET}-input`).
5. Press the hotkey again to release capture; events resume flowing to the game.

### Limitations (v1)

- **Listener substitution only** (not `wl_proxy_add_dispatcher`). Catches SDL, GLFW, and most native games. GTK/Qt-style clients that use dispatchers will not be intercepted — a warning is logged.
- **Qt WebEngine injection is stubbed**: the `InputReceiver` in the webview connects to the input socket and logs events (set `IDK_DEBUG=1`), but does not yet inject them into the QWebEngineView. This is the next task.
- **Native Wayland games only**. X11 games (running under XWayland) don't call `wl_proxy_add_listener` — they use X11 APIs. An X11 input hook would be a separate feature.
- **xkbcommon is optional**. If `libxkbcommon.so.0` isn't available, hotkey detection falls back to raw evdev scancodes (KEY_F8 = 66) and the `keysym` field is 0 in forwarded events. The webview can still decode scancodes itself.

### Input IPC protocol

Separate socket from the frame socket, no `SCM_RIGHTS`. Path: `${IDK_SOCKET}-input` (default: `/tmp/idk-overlay-<pid>-input`).

Each message is 64 bytes:

```
+----------------------+
| magic    uint32      |  0x49444B49 ("IDKI")
| type     uint32      |  1=KEY, 2=BUTTON, 3=MOTION, 4=AXIS, 5=STATE
| time     uint32      |  wayland timestamp (ms)
| serial   uint32      |  wayland serial
| keycode  uint32      |  evdev scancode (KEY events)
| keysym   uint32      |  xkb keysym (KEY events, 0 if xkb unavailable)
| state    uint32      |  0=release, 1=press (KEY/BUTTON)
| button   uint32      |  BTN_* code (BUTTON events)
| x        int32       |  surface-local x in pixels (MOTION)
| y        int32       |  surface-local y in pixels (MOTION)
| dx       int32       |  scroll delta (AXIS, axis=1)
| dy       int32       |  scroll delta (AXIS, axis=0)
| mods     uint32      |  Ctrl=1, Shift=2, Alt=4, Super=8
| capture  uint32      |  0=passing through, 1=captured
| reserved uint32      |  future use
| checksum uint32      |  CRC32 of all preceding fields
+----------------------+
```

The webview connects to this socket and reads events with `idk_ipc_recv_input()`. See `include/public/idk_ipc.h` for the wire format and `src/cli/webview/input_receiver.cpp` for an example receiver.

## Project structure

```
src/
├── core/           Compositor engine (shared)
├── gl/             GL/EGL loader + shader loader
├── hook/           EGL, GLX, Vulkan, Wayland input hooks + overlay init
├── shim/           elfhacks (MangoHud's ELF symbol resolver)
├── ipc/            Unix socket SCM_RIGHTS + input event IPC
├── render/         idk-render process
├── lib/            libidk-framesource (frame sender)
└── cli/            idk-inject, idk-webview (incl. InputReceiver)

include/
├── public/         Public API headers (idk_fs.h, idk_ipc.h)
├── core/           Internal: compositor, log
├── gl/             Internal: GL loader, shaders
├── hook/           Internal: EGL, GLX, Vulkan, overlay, wayland_input +
│                   wayland_input_types.h (vendored protocol structs)
├── shim/           Internal: elfhacks
└── render/         Internal: receive
```

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
