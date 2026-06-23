# idk-overlay

**Open-source, Wayland-native game overlay platform for Linux.**

Injects into a target process via [syringe](https://github.com/K4zoku/syringe) (ptrace / LD_PRELOAD / .NET diagnostic IPC), hooks graphics present functions, and composites a webview-rendered overlay on top of the game's framebuffer.

## Features

- **3 injection paths**: LD_PRELOAD (universal), ptrace (live process), .NET diagnostic IPC (bypasses anti-debug)
- **Wayland-native**: works on Wayland without X11 fallback
- **Live process injection**: inject into already-running games (not just LD_PRELOAD)
- **.NET support**: injects into osu! lazer via diagnostic IPC — no ptrace, no root, no anti-debug bypass
- **Qt6 WebEngine webview**: render HTML/CSS/JS overlays
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

## Project structure

```
src/
├── core/           Compositor engine (shared)
├── gl/             GL/EGL loader + shader loader
├── hook/           EGL, GLX, Vulkan hooks + overlay init
├── shim/           elfhacks (MangoHud's ELF symbol resolver)
├── ipc/            Unix socket SCM_RIGHTS
├── render/         idk-render process
├── lib/            libidk-framesource (frame sender)
└── cli/            idk-inject, idk-webview

include/
├── public/         Public API headers (idk_fs.h, idk_ipc.h)
├── core/           Internal: compositor, log
├── gl/             Internal: GL loader, shaders
├── hook/           Internal: EGL, GLX, Vulkan, overlay
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
