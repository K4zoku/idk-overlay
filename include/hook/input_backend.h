#ifndef IDK_INPUT_BACKEND_H
#define IDK_INPUT_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#include "public/idk_input.h"

/*
 * Input capture backend abstraction: captures the game's keyboard/mouse
 * input, forwards it to the webview over the input socket, and detects the
 * capture/overlay hotkeys.
 *
 * Concrete backends live in src/hook/input/<name>/ — wayland and x11.
 * Wayland and x11 already expose these operations under per-backend names
 * (idk_wayland_input_* / idk_x11_input_*); this interface is the shared
 * contract.
 */
typedef struct input_backend {
  const char *name;
  /* Install the backend's hooks. Returns 0 on success, -1 if the
   * backend is unavailable (e.g. no wayland display). */
  int (*init)(void);
  void (*shutdown)(void);
  /* Enable/disable capture forwarding to the webview. */
  void (*set_capture)(int enable);
  int (*is_captured)(void);
} input_backend_t;

int idk_input_cursor_snapshot(uint32_t known_generation, idk_cursor_update_t *cursor, uint8_t *pixels, size_t capacity,
                              uint32_t *generation);
void idk_input_cursor_dispatch(void);

#endif /* IDK_INPUT_BACKEND_H */
