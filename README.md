# idk-overlay

## 💡 About

This project started as a workaround for a simple problem: [tosu](https://github.com/tosuapp/tosu)'s built-in ingame overlay doesn't support Linux, leaving Linux players without live stats in osu!. What began as a targeted fix eventually grew into something more general - a full overlay engine that can hook into any game and display any webpage on top of it.

`idk-overlay` lets you show a custom overlay (stats, chat, widgets, anything you can build with HTML/CSS/JS) on top of any game running on Linux. It works by injecting a small library into the game, intercepting the moment the game draws a frame, and compositing your overlay on top before the frame is presented.

### ✨ Features

- [x] Three ways to attach to a game - pick whichever fits:
  - [x] `LD_PRELOAD` - easiest, works with most games.
  - [x] `ptrace` injection - attach to a game that's already running.
  - [x] .NET diagnostic IPC - for .NET games (e.g. osu!lazer) where anti-debug blocks ptrace.
- [x] Display any webpage you want.
- [x] Input capture support:
  - [x] Works on Wayland.
  - [x] Works on X11.
- [x] Hotkeys.
  - [x] Toggle overlay visibility (default: `F8`).
  - [x] Toggle keyboard/mouse capture so you can click and type into the overlay (default: `Shift+Tab`).
- [x] Inject your own custom JS scripts into the overlay webpage.
  - [x] Custom events are emitted whenever overlay visibility or input capture is toggled, so your scripts can react to those state changes - see [Custom JS injection](#-custom-js-injection) below.
- [x] Supports both OpenGL/EGL and Vulkan games.
- [x] Zero-copy frame sharing between the webview and the game (DMABUF), so it's fast and doesn't add visible lag.
- [x] Lazy rendering - the webview only paints a new frame after the game acknowledges the previous one (ACK), so it never renders faster than the game can consume and wastes no CPU/GPU on frames that haven't been displayed yet.
- [x] Falls back to SHM automatically whenever DMABUF isn't available, or fails at any stage.
- [x] No flickering or screen tearing - double-buffered rendering.
- [x] Plugin-based - adding support for a new graphics backend doesn't require touching the core.

## 🧾 Dependencies

- EGL + OpenGL, Vulkan
- Qt6 WebEngine (only needed for the webview client)
- [syringe](https://github.com/K4zoku/syringe) - injection toolkit (bundled as a subproject)
- Linux x86-64 (aarch64 builds, but is untested)

## 📥 Installation

### Arch Linux

PKGBUILD available at [dist/PKGBUILD](dist/PKGBUILD)

```bash
git clone https://github.com/K4zoku/idk-overlay
cd idk-overlay/dist
makepkg -si
```

_AUR package coming soon_

### Other distributions

No official packages yet. See [Building from source](#%EF%B8%8F-building-from-source) below.

If you can port `idk-overlay` to other package managers, contributions are welcome!

### 🏗️ Building from source

#### Build dependencies

- meson
- ninja
- gcc
- pkg-config
- libglvnd
- libegl
- mesa
- vulkan-icd-loader
- qt6-base
- qt6-webengine

On Arch Linux:

```sh
sudo pacman -S meson ninja gcc pkg-config libglvnd libegl mesa vulkan-icd-loader
```

#### Build

```sh
meson setup build
meson compile -C build
```

#### Install

```sh
sudo meson install -C build
```

## 📜 Usage

You don't need to launch the webview yourself. As soon as the library is injected into the game, it automatically forks a child process for the webview and calls `prctl(PR_SET_PDEATHSIG, SIGTERM)` on it, so the webview always shuts down together with the game rather than being left running on its own.

### How process filtering works

When using `LD_PRELOAD`, the library is injected into **every** process spawned by the launcher — wrapper scripts, AppImage helpers, Wine processes (`wineserver`, `wineboot`), etc. Without filtering, each one would fork a webview + open SHM → OOM.

The library uses **GL library detection** as the filter: it polls for `libGL.so`, `libEGL.so`, or `libvulkan.so` to be loaded (via `dlopen(RTLD_NOLOAD)`). Only when one of these is detected does it fork the webview and install hooks. Wrapper scripts and Wine helpers never load GL libraries, so they're automatically filtered out.

This is more reliable than process-name filtering because:
- Wine's process name changes from `wine` to `osu!.exe` **after** the constructor runs (timing issue)
- AppImage extractors have unpredictable names
- GL detection has no timing issue — the game loads GL when it's ready to render

### Vulkan games - layer (required)

Vulkan doesn't support late injection the way OpenGL/EGL does, so for Vulkan games the overlay has to be attached through a Vulkan layer from the moment the process starts, instead of being injected afterwards:

```sh
VK_LAYER_PATH=/path/to/libidk-overlay/directory VK_INSTANCE_LAYERS=VK_LAYER_IDK_overlay vkcube
```

### OpenGL/EGL games - two options

OpenGL/EGL games are more flexible: you can either preload the library at launch, or attach to a process that's already running.

**Option 1: LD_PRELOAD**

```sh
LD_PRELOAD=/path/to/libidk-overlay.so eglgears_wayland
```

**Option 2: Inject into a running process (recommended for osu!lazer)**

```sh
osu-lazer &
idk-inject $(pgrep osu!)  # actual osu!lazer process name
# Auto-detects .NET games and uses diagnostic IPC (no ptrace, no root needed).
# Falls back to ptrace for native apps.
```

### Controls

- `F8` - show/hide the overlay.
- `Shift+Tab` - capture keyboard/mouse into the overlay (press again to release back to the game).

### ⚙️ Environment variables

| Variable | Default | Description |
|----------|---------|-------------|
| `IDK_SOCKET` | `$XDG_RUNTIME_DIR/idk-overlay-<pid>` (falls back to `/tmp/idk-overlay-<pid>`) | Unix socket path used to talk to the injected game. |
| `IDK_VK` | `1` | Enable Vulkan hooks. |
| `IDK_GL` | `1` | Enable OpenGL/EGL hooks. |
| `IDK_DEBUG` | (unset) | Set to `1` for debug logging. |
| `IDK_HOTKEY_CAPTURE` | `Shift+Tab` | Hotkey to toggle input capture. |
| `IDK_HOTKEY_OVERLAY` | `F8` | Hotkey to toggle overlay visibility. |

### 🛠️ Configuration

The environment variables above are enough for a single overlay on a single game, but if you want different URLs, sizes, or hotkeys depending on which game is running, you can drop a config file at `$XDG_CONFIG_HOME/idk-overlay.conf` (or `~/.config/idk-overlay.conf` if that variable isn't set).

Each `[Section]` in the file describes one overlay. When the injected library forks the webview, it passes along the current process name (read from `/proc/self/comm`), and the webview only loads the sections whose `Match=` regex matches that name - if no section in the file defines `Match=`, every section is loaded instead.

| Key | Description |
|-----|--------------|
| `Match=<regex>` | Process name regex this section applies to. If any section in the file sets this, sections without it are skipped. |
| `Url=<url>` | The page to render - a web page, a local file, or your own tool (e.g. tosu's API endpoint). |
| `Width=<px>` / `Height=<px>` | Initial size of the overlay webview. |
| `HotkeyCapture=<combo>` / `HotkeyOverlay=<combo>` | Per-game overrides for the `IDK_HOTKEY_CAPTURE` / `IDK_HOTKEY_OVERLAY` env vars above. |
| `InjectScripts=<file[,file...]>` | One or more local JS files to inject into the page after it loads - see [Custom JS injection](#-custom-js-injection) below. |

Example config, defining one overlay for osu! and one for glxgears, plus a commented-out catch-all section:

```ini
; idk-overlay configuration file
; Place at $XDG_CONFIG_HOME/idk-overlay.conf or ~/.config/idk-overlay.conf
;
; ── Overlay sections ────────────────────────────────────────────────────
; Each [Section_Name] group defines a webview overlay.
; Section name is arbitrary (can contain special chars in Qt INI format).
;
; Match=<regex>  - Process name regex. When the injected lib forks
;                  webview, it passes --match <comm> (from /proc/self/comm).
;                  If ANY section has Match=, only matching sections load.
;                  Sections without Match= are skipped when filtering.
;                  If NO section has Match=, all sections load.
;
; Url=<url>      - URL to render (web page, local file, etc.)
; Width=<px>     - Initial width
; Height=<px>    - Initial height

; Example: overlay for osu! with tosu overlay (process name "osu!")
[osu]
Match=osu.*
Url=http://127.0.0.1:24050/api/ingame
Width=1280
Height=720
; Hotkeys (optional, override env IDK_HOTKEY_CAPTURE / IDK_HOTKEY_OVERLAY)
HotkeyCapture=Shift+Tab
HotkeyOverlay=F8
; If both same: press→capture ON+show, press→capture OFF (overlay stays)
InjectScripts=tosu.js

; Example: overlay for glxgears
[glxgears]
Match=glxgears
Url=https://example.com
Width=800
Height=600

; Example: catch-all overlay (no Match=, loads when no section matches)
; [default]
; Url=https://example.com
; Width=1024
; Height=768
```

### 📜 Custom JS injection

Any file listed in `InjectScripts=` is loaded into the overlay's webview after the page itself finishes loading, so it can run alongside whatever the page already does. This is the place to bridge your own page's logic with idk-overlay's state - for example, pausing animations or showing an "editing" banner while the user has input capture turned on.

To make that possible, idk-overlay dispatches custom `CustomEvent`s on `window` whenever overlay visibility or input capture changes:

| Event | Fires when... |
|-------|----------------|
| `overlaycapturestart` | Input capture is turned on. |
| `overlaycaptureend` | Input capture is turned off. |
| `overlaycapturechanged` | Either of the above - `event.detail.captured` tells you the new state (`true`/`false`). |
| `overlayshow` | The overlay becomes visible. |
| `overlayhide` | The overlay is hidden. |
| `overlayvisiblechanged` | Either of the above - `event.detail` describes the new visibility state. |

Example - `tosu.js`, which listens for capture changes and notifies the rest of the page via `postMessage`:

```js
window.addEventListener("overlaycapturechanged", (event) => {
  const captured = event.detail.captured;
  if (captured) {
    window.postMessage("editingStarted");
  } else {
    window.postMessage("editingEnded");
  }
});
```

### 🧪 Tested on

| Target | LD_PRELOAD | ptrace | .NET IPC | Vulkan Layer
|--------|------------|--------|----------|------------|
| glmark2-egl | ✅ | ✅ | N/A | N/A |
| osu! lazer (.NET) | ⚠️* | ❌ (anti-debug) | ✅ | N/A |
| eglgears_wayland | ✅ | ✅ | N/A | N/A |
| vkcube | N/A | N/A | N/A | ✅ |

\* For osu!lazer: `LD_PRELOAD` works when launching the osu!lazer binary directly. It does **not** work through osu!lazer's wrapper script or AppImage - when going through those, the hook does get installed, but it also gets applied to every child process spawned within, causing a cascade of issues (not yet fixed; likely requires a process-whitelist feature). Until then, injecting into the running process (Option 2 above) is the recommended approach for osu!lazer.

---

## 🏗️ Architecture

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

The overlay is rendered out-of-process by the webview client and shared with the game via a zero-copy DMABUF (or SHM fallback) over a Unix socket. The game-side library hooks the graphics swap/present call, imports the latest overlay frame as a texture, draws it as a quad on top of the game's framebuffer, then calls the original swap function so the game behaves normally.

### Components

| Component | Description |
|-----------|-------------|
| `libidk-overlay.so` | Injectable library - hooks EGL/GLX/Vulkan, composites overlay |
| `libidk-framesource.so` | Frame sender library (used by webview) |
| `idk-webview` | Qt6 WebEngine webview client |
| `idk-inject` | CLI - injects libidk-overlay.so into running process |
| `bin/idk-overlay` | Wrapper script - launches game with overlay (WIP, not used in the usage examples above yet) |

### Injection path detection

The library auto-detects whether it was loaded via `LD_PRELOAD` or late-injected:

- **LD_PRELOAD**: the dynamic linker has already resolved the hooked symbol to our address (`RTLD_DEFAULT` matches our function pointer) - no syringe GOT patching needed.
- **Late inject**: the real function is already resolved; syringe patches GOT/PLT to redirect calls to our implementation.

Detection is a single `dlsym(RTLD_DEFAULT, sym) == our_func` check in `hook/hook_util.h`.

### Plugin interface

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

### Project structure

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
│   ├── overlay.c            Orchestrator - background polling, plugin discovery
│   ├── egl_hook.c           EGL swap hook plugin
│   ├── glx_hook.c           GLX swap hook plugin
│   ├── vulkan_hook.c        Vulkan syringe hook plugin
│   ├── vulkan_layer.c       Vulkan layer (VK_LAYER_PATH)
│   ├── x11/                 X11 input capture (3 files)
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

### Technical highlights

- **DMABUF import**: zero-copy texture import via EGL (`EGL_EXT_image_dma_buf`) or `GL_EXT_memory_object` (GLX fallback).
- **ACK flow control**: webview frame rate is synced to the game's swap rate, so it never renders faster than needed.
- **GL state save/restore**: comprehensive, based on the MangoHud approach, so the overlay never corrupts the game's GL state.
- **Program auto-recovery**: detects when the GL driver reuses program IDs and re-initializes shaders automatically.
- **SPIR-V shaders**: used when available, with GLSL as a fallback.

---

## 🚌 IPC Protocol

### Frame transport (overlay socket)

Socket path: `$IDK_SOCKET` (see environment variables above). The webview client sends rendered frames as a SHM or DMABUF file descriptor over `SCM_RIGHTS`, with a 28-byte frame header (size, format, stride, etc.). The game-side library imports the buffer as a texture and replies with a 16-byte ACK once it's consumed, which also doubles as the resize/flow-control signal.

**Frame header (28 bytes)** - sent with the buffer fd:

```
+------------------------------------------------------+
| modifier uint64   |  DRM modifier (0=linear/SHM)     |
| width    uint32   |  Buffer width (px)               |
| height   uint32   |  Buffer height (px)              |
| stride   uint32   |  Bytes per row (DMABUF), 0=SHM   |
| fourcc   uint32   |  DRM fourcc (DMABUF), 0=SHM      |
| flags    uint8    |  bit0=visible, bit1=dmabuf       |
| _pad     uint8[3] |  reserved                        |
+------------------------------------------------------+
```

**ACK message (16 bytes)** - sent back by the game once the frame is consumed; also signals the webview to resize if the game's window size changed:

```
+-------------------------------------------------------+
| ack   uint8     |  0=accepted, 1=rejected (force SHM) |
| w     int32     |  game width (0 = no resize)         |
| h     int32     |  game height (0 = no resize)        |
| _pad  uint8[3]  |  reserved (alignment padding)       |
+-------------------------------------------------------+
```

### Input IPC protocol

Separate socket from the frame socket (no `SCM_RIGHTS`). Path: `${IDK_SOCKET}-input`.

Used when input capture is toggled (default hotkey `Shift+Tab`) to forward keyboard/mouse events from the game to the webview instead of the game.

Each message is 20 bytes (`idk_input_event_t`):

```
+----------------------------------------------------+
| type   uint8   |  KEY/BUTTON/MOTION/AXIS/STATE     |
| flags  uint8   |  bit0=press, bit1=capture         |
| mods   uint16  |  Ctrl=1, Shift=2, Alt=4, Super=8  |
| time   uint32  |  Input timestamp (ms)             |
| union  (12B)   |  key { keycode, keysym, _pad }    |
|                |  btn { x, y, button }             |
|                |  motion { x, y, _pad }            |
|                |  axis { dx, dy, _pad }            |
|                |  repeat { rate, delay, _pad }     |
+----------------------------------------------------+
```

---
> <p align="center">Made with ❤️ by <a href="https://github.com/K4zoku">@K4zoku</a></p>
