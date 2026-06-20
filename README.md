# idk-overlay

**Zero-copy framebuffer capture for overlay rendering via a separate process.**

Injects into a target process via [syringe](https://github.com/K4zoku/syringe) (ptrace + GOT patching + trampoline), captures frames through Vulkan dmabuf (zero-copy) or OpenGL SHM fallback, and sends them to a separate render process over Unix domain socket with `SCM_RIGHTS`.

## Architecture

```
┌───────────────────────┐                ┌───────────────────────┐
│  Target Process       │  Unix socket   │  Render Process       │
│  (game/app)           │  SCM_RIGHTS    │  (idk-render)         │
│                       │   <----------  │                       │
│  ┌─────────────────┐  │   fd + meta    │  ┌─────────────────┐  │
│  │ libidk-overlay  │  │                │  │ Vulkan/OpenGL   │  │
│  │  (injected .so) │  │                │  │  (imports fd)   │  │
│  │                 │  │                │  │                 │  │
│  │ Hooks:          │  │                │  │ Reads pixels:   │  │
│  │ - vkQueuePresent│  │                │  │ - dmabuf import │  │
│  │ - glXSwapBuffers│  │                │  │ - EGLImage      │  │
│  │ - eglSwapBuffers│  │                │  │ - SHM mmap      │  │
│  │                 │  │                │  │ (alpha kept)    │  │
│  └─────────────────┘  │                │  └─────────────────┘  │
└───────────────────────┘                └───────────────────────┘
         syringe_inject()                          idk-render
```

## Components

### libidk-overlay.so
Injectable shared library. Hooks present/swap functions, exports frames as dmabuf fd (Vulkan) or SHM (OpenGL), sends over Unix socket.

### idk-render
Separate render process. Receives frames, imports dmabuf/SHM, reads pixel data (preserves alpha channel).

### idk-inject
CLI wrapper. Starts `idk-render`, then calls `syringe_inject()` to load `libidk-overlay.so` into the target process.

## Build

```bash
# 1. Ensure syringe is set up as subproject
ln -s ../../syringe subprojects/syringe

# 2. Build
meson setup build
meson compile -C build

# 3. Install (optional)
meson install -C build
```

## Usage

### Option 1: Using idk-inject (automatic)

```bash
# Start the target process first
./my_game &
TARGET_PID=$!

# Inject and start render process
./build/idk-inject $TARGET_PID

# Or with options:
./build/idk-inject $TARGET_PID --socket /tmp/idk-custom --vk 1 --gl 0
```

### Option 2: Manual

```bash
# Start render process
./build/idk-render --socket /tmp/idk-overlay-1234 &
RENDER_PID=$!

# Inject into target
./build/idk-inject 1234 ./build/libidk-overlay.so --socket /tmp/idk-overlay-1234
```

### Environment variables

| Variable | Default | Description |
|----------|---------|-------------|
| `IDK_VK` | `1` | Enable Vulkan hooks |
| `IDK_GL` | `1` | Enable OpenGL hooks |
| `IDK_SOCKET` | `/tmp/idk-overlay-<pid>` | Unix socket path |

## How it works

### Frame capture flow (Vulkan)

1. Target calls `vkQueuePresentKHR`
2. Hook intercepts → extracts `VkImage` from swapchain
3. Exports `VkImage` as dmabuf fd via `VK_KHR_external_memory_fd`
4. Sends `fd + metadata (width, height, stride, format)` over Unix socket
5. Render process receives fd, imports via `VK_EXT_image_drm_image_format`

### Frame capture flow (OpenGL)

1. Target calls `glXSwapBuffers` / `eglSwapBuffers`
2. Hook intercepts → reads pixels via `glReadPixels`
3. Writes to `/dev/shm/idk-overlay-<pid>-XXXXXX`
4. Sends SHM fd over Unix socket
5. Render process receives fd, `mmap()`s it for pixel data

## TODO

- [ ] Full Vulkan dmabuf export (currently stub)
- [ ] Full OpenGL SHM readback (currently stub)
- [ ] Vulkan import in idk-render
- [ ] EGLImage import in idk-render
- [ ] SHM mmap in idk-render
- [ ] Frame saving to PNM/PPM/PGM
- [ ] DRM plane format detection
- [ ] Multi-plane format support (YUV)
- [ ] Performance: frame pacing, throttling

## Dependencies

- Linux x86-64
- Vulkan ICD (`libvulkan.so`)
- OpenGL (libGL, libEGL)
- Wayland/X11 (optional, for display)
- syringe (subproject)

## License

MIT
