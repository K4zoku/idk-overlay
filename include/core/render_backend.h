#ifndef IDK_RENDER_BACKEND_H
#define IDK_RENDER_BACKEND_H

/*
 * Render backend abstraction: composites the overlay frame (received from
 * the producer via transport) into the game's frame.
 *
 * Concrete backends live in src/core/render/<name>/ and are built into the
 * library that matches their API: GL/EGL in libidk-overlay.so (LD_PRELOAD
 * path), Vulkan in libidk-vklayer.so (Vulkan layer). A backend is selected
 * at build time today.
 */
typedef struct render_backend {
  const char *name;
  /* Composite the current overlay frame into the game frame.
   * The game's rendering context must be current. Returns 0 on
   * success, -1 when there is nothing to render. */
  int (*render)(void);
  /* Forward a game resize so the next frame is composed correctly. */
  void (*notify_resize)(int w, int h);
  /* Release all backend resources (called at game shutdown). */
  void (*shutdown)(void);
} render_backend_t;

#endif /* IDK_RENDER_BACKEND_H */
