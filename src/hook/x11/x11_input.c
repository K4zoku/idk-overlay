/* x11_input.c — X11 input hook for overlay input capture.
 *
 * Hooks XNextEvent-family functions to intercept the game's X11 event loop.
 * When capture is toggled on (Shift+Tab), keyboard/mouse events are
 * swallowed from the game and forwarded to the webview via IPC.
 *
 * SHARES state with the Wayland input hook:
 *   - g_captured, g_mods, g_hotkey_* — capture state + hotkey config
 *   - g_repeat_rate, g_repeat_delay — keyboard repeat info
 *   - init_input_socket, send_event_to_webview, send_capture_state,
 *     send_repeat_info, teardown_input_socket — IPC socket layer
 *   - configure_hotkey, is_hotkey — hotkey parsing/detection
 *
 * Wayland and X11 are mutually exclusive in practice (games use one or
 * the other), but both hook sets can coexist — whichever library is
 * loaded installs its hook. The shared state ensures only one capture
 * toggle exists at runtime.
 */

#include "hook/x11_internal.h"
#include "hook/syringe_hook.h"
#include "hook/hook_util.h"

/* ── X11-specific globals (NOT shared with Wayland) ──────────────────── */
void *g_x11_handle = NULL;
int g_hook_installed = 0;
Display *g_game_display = NULL;
Window  g_game_window = 0;
Cursor  g_blank_cursor = 0;
Cursor  g_saved_cursor = 0;
int     g_cursor_grabbed = 0;

XNextEvent_fn               orig_XNextEvent = NULL;
XPeekEvent_fn               orig_XPeekEvent = NULL;
XCheckWindowEvent_fn        orig_XCheckWindowEvent = NULL;
XMaskEvent_fn               orig_XMaskEvent = NULL;
XCheckMaskEvent_fn          orig_XCheckMaskEvent = NULL;
XCheckTypedEvent_fn         orig_XCheckTypedEvent = NULL;
XCheckTypedWindowEvent_fn   orig_XCheckTypedWindowEvent = NULL;
XWindowEvent_fn             orig_XWindowEvent = NULL;
XPending_fn                 orig_XPending = NULL;
XEventsQueued_fn            orig_XEventsQueued = NULL;
XSelectInput_fn             orig_XSelectInput = NULL;
XGetWindowAttributes_fn     fn_XGetWindowAttributes = NULL;

XCreatePixmapCursor_fn  fn_XCreatePixmapCursor = NULL;
XFreePixmap_fn          fn_XFreePixmap = NULL;
XDefineCursor_fn        fn_XDefineCursor = NULL;
XFreeCursor_fn          fn_XFreeCursor = NULL;
XGrabPointer_fn         fn_XGrabPointer = NULL;
XUngrabPointer_fn       fn_XUngrabPointer = NULL;
XKeycodeToKeysym_fn     fn_XKeycodeToKeysym = NULL;
XStringToKeysym_fn      fn_XStringToKeysym = NULL;
XFlush_fn               fn_XFlush = NULL;
XSync_fn                fn_XSync = NULL;

/* ── Symbol resolution ───────────────────────────────────────────────── */

static int resolve_x11_symbols(void) {
    if (g_x11_handle) return 0;

    void *h = dlopen("libX11.so.6", RTLD_NOW | RTLD_NOLOAD);
    if (!h) h = dlopen("libX11.so.6", RTLD_NOW);
    if (!h) h = dlopen("libX11.so", RTLD_NOW);
    if (!h) {
        XERR("dlopen libX11 failed: %s", dlerror());
        return -1;
    }
    g_x11_handle = h;

    fn_XCreatePixmapCursor = (XCreatePixmapCursor_fn)dlsym(h, "XCreatePixmapCursor");
    fn_XFreePixmap         = (XFreePixmap_fn)dlsym(h, "XFreePixmap");
    fn_XDefineCursor       = (XDefineCursor_fn)dlsym(h, "XDefineCursor");
    fn_XFreeCursor         = (XFreeCursor_fn)dlsym(h, "XFreeCursor");
    fn_XGrabPointer        = (XGrabPointer_fn)dlsym(h, "XGrabPointer");
    fn_XUngrabPointer      = (XUngrabPointer_fn)dlsym(h, "XUngrabPointer");
    fn_XKeycodeToKeysym    = (XKeycodeToKeysym_fn)dlsym(h, "XKeycodeToKeysym");
    fn_XStringToKeysym     = (XStringToKeysym_fn)dlsym(h, "XStringToKeysym");
    fn_XFlush              = (XFlush_fn)dlsym(h, "XFlush");
    fn_XSync               = (XSync_fn)dlsym(h, "XSync");
    fn_XGetWindowAttributes = (XGetWindowAttributes_fn)dlsym(h, "XGetWindowAttributes");

    if (!fn_XStringToKeysym) {
        XERR("XStringToKeysym not resolved\n");
        return -1;
    }

    XLOG("libX11 resolved: XKeycodeToKeysym=%p XStringToKeysym=%p XDefineCursor=%p XGrabPointer=%p",
         (void*)fn_XKeycodeToKeysym, (void*)fn_XStringToKeysym,
         (void*)fn_XDefineCursor, (void*)fn_XGrabPointer);
    return 0;
}

/* ── Capture toggle ──────────────────────────────────────────────────── */

void idk_x11_input_set_capture(int enable) {
    int new_state = enable ? 1 : 0;
    if (new_state == g_captured) return;

    g_captured = new_state;
    XLOG("set_capture(%s)", new_state ? "ON" : "OFF");

    /* No XGrabPointer — our XNextEvent-family hooks already intercept events
     * before the game sees them. Grabbing would lock the cursor into the game
     * window (relative mouse mode), which is NOT what we want for an overlay.
     * The game continues to receive non-input events (Expose, ConfigureNotify,
     * etc.) normally; we only swallow KeyPress/KeyRelease/ButtonPress/
     * ButtonRelease/MotionNotify when captured. */

    send_capture_state((uint32_t)new_state);
    if (new_state) send_repeat_info();
}

int idk_x11_input_is_captured(void) {
    return g_captured;
}

/* ── Event dispatch ──────────────────────────────────────────────────── */

/* Forward declarations — defined in x11_kb.c / x11_ptr.c */
extern int x11_handle_key_event(XEventStorage *ev);
extern int x11_handle_button_event(XEventStorage *ev);
extern int x11_handle_motion_event(XEventStorage *ev);

/* Dispatch a single X event. Returns 1 if it should be swallowed
 * (captured or hotkey), 0 if it should be returned to the caller. */
int x11_dispatch_event(XEventStorage *ev) {
    if (!ev) return 0;
    int type = ev->xany.type;

    /* Cache display + window from any event */
    if (!g_game_display && ev->xany.display) g_game_display = ev->xany.display;
    if (!g_game_window && ev->xany.window) g_game_window = ev->xany.window;

    /* Retroactively inject pointer + key release masks on the game window.
     * Games like glxgears call XSelectInput BEFORE our hook installs, so
     * our XSelectInput hook never fires. We need to OR-in our masks here
     * so ButtonPress/ButtonRelease/MotionNotify/KeyRelease events arrive.
     * Done once per process (g_masks_injected flag). */
    static int g_masks_injected = 0;
    if (!g_masks_injected && g_game_display && g_game_window &&
        fn_XGetWindowAttributes && orig_XSelectInput) {
        g_masks_injected = 1;

        /* XWindowAttributes is a large struct. We only need your_event_mask.
         * On x86-64 Linux, it's at offset 104 (long). Use a raw buffer. */
        long attrs[64];  /* oversized to be safe */
        if (fn_XGetWindowAttributes(g_game_display, g_game_window, attrs) != 0) {
            long cur_mask = attrs[13];  /* your_event_mask at offset 104 = 13*8 */
            long new_mask = cur_mask | ButtonPressMask | ButtonReleaseMask |
                            PointerMotionMask | KeyReleaseMask;
            orig_XSelectInput(g_game_display, g_game_window, new_mask);
            XLOG("retroactively injected event masks: 0x%lx -> 0x%lx on window 0x%lx",
                 cur_mask, new_mask, (unsigned long)g_game_window);
        } else {
            /* Fallback: just set our masks (may lose game's masks) */
            long extra = ButtonPressMask | ButtonReleaseMask |
                         PointerMotionMask | KeyReleaseMask;
            orig_XSelectInput(g_game_display, g_game_window, extra);
            XLOG("retroactively set event masks (fallback): 0x%lx on window 0x%lx",
                 extra, (unsigned long)g_game_window);
        }
    }

    switch (type) {
        case KeyPress:
        case KeyRelease:
            return x11_handle_key_event(ev);

        case ButtonPress:
        case ButtonRelease:
            return x11_handle_button_event(ev);

        case MotionNotify:
            return x11_handle_motion_event(ev);

        default:
            return 0;  /* don't swallow other event types */
    }
}

/* ── Hooks ───────────────────────────────────────────────────────────── */

/* Generic wrapper: call orig, if it returned an event, dispatch it.
 * If dispatch says "swallow", loop to get the next event (for blocking
 * variants). For non-blocking (XCheck*) variants, return 0 to indicate
 * "no event available". */
/* Fill ev with a harmless NoExpose event so the game's main loop continues.
 * Games like glxgears block on XNextEvent — if we swallow the event and
 * loop back to orig_XNextEvent, it blocks again, preventing the game from
 * reaching glXSwapBuffers (→ compositor never processes frames → ACK timeout).
 * Returning a NoExpose event lets the game's loop proceed to render+swap. */
static void fill_noexpose(XEventStorage *ev, Display *dpy) {
    memset(ev, 0, sizeof(*ev));
    ev->xany.type = NoExpose;  /* 14 */
    ev->xany.display = dpy;
}

static int hook_XNextEvent(Display *dpy, XEventStorage *ev) {
    if (!orig_XNextEvent)
        orig_XNextEvent = (XNextEvent_fn)hook_orig("XNextEvent");
    if (!g_game_display) g_game_display = dpy;

    int r = orig_XNextEvent(dpy, ev);
    if (r != 0) return r;
    if (x11_dispatch_event(ev)) {
        /* Event swallowed (captured/hotkey) — return NoExpose instead of
         * looping, so the game's main loop proceeds to render + swap. */
        fill_noexpose(ev, dpy);
    }
    return 0;
}

static int hook_XPeekEvent(Display *dpy, XEventStorage *ev) {
    if (!orig_XPeekEvent)
        orig_XPeekEvent = (XPeekEvent_fn)hook_orig("XPeekEvent");
    if (!g_game_display) g_game_display = dpy;

    int r = orig_XPeekEvent(dpy, ev);
    if (r != 0) return r;
    x11_dispatch_event(ev);
    return 0;
}

static int hook_XCheckWindowEvent(Display *dpy, Window w, long mask, XEventStorage *ev) {
    if (!orig_XCheckWindowEvent)
        orig_XCheckWindowEvent = (XCheckWindowEvent_fn)hook_orig("XCheckWindowEvent");
    if (!g_game_display) g_game_display = dpy;
    if (!g_game_window && w) g_game_window = w;

    int r = orig_XCheckWindowEvent(dpy, w, mask, ev);
    if (r == 0) return 0;
    if (x11_dispatch_event(ev)) {
        fill_noexpose(ev, dpy);
    }
    return 1;
}

static int hook_XMaskEvent(Display *dpy, long mask, XEventStorage *ev) {
    if (!orig_XMaskEvent)
        orig_XMaskEvent = (XMaskEvent_fn)hook_orig("XMaskEvent");
    if (!g_game_display) g_game_display = dpy;

    int r = orig_XMaskEvent(dpy, mask, ev);
    if (r != 0) return r;
    if (x11_dispatch_event(ev)) {
        fill_noexpose(ev, dpy);
    }
    return 0;
}

static int hook_XCheckMaskEvent(Display *dpy, long mask, XEventStorage *ev) {
    if (!orig_XCheckMaskEvent)
        orig_XCheckMaskEvent = (XCheckMaskEvent_fn)hook_orig("XCheckMaskEvent");
    if (!g_game_display) g_game_display = dpy;

    int r = orig_XCheckMaskEvent(dpy, mask, ev);
    if (r == 0) return 0;
    if (x11_dispatch_event(ev)) {
        fill_noexpose(ev, dpy);
    }
    return 1;
}

static int hook_XCheckTypedEvent(Display *dpy, int type, XEventStorage *ev) {
    if (!orig_XCheckTypedEvent)
        orig_XCheckTypedEvent = (XCheckTypedEvent_fn)hook_orig("XCheckTypedEvent");
    if (!g_game_display) g_game_display = dpy;

    int r = orig_XCheckTypedEvent(dpy, type, ev);
    if (r == 0) return 0;
    if (x11_dispatch_event(ev)) {
        fill_noexpose(ev, dpy);
    }
    return 1;
}

static int hook_XCheckTypedWindowEvent(Display *dpy, Window w, int type, XEventStorage *ev) {
    if (!orig_XCheckTypedWindowEvent)
        orig_XCheckTypedWindowEvent = (XCheckTypedWindowEvent_fn)hook_orig("XCheckTypedWindowEvent");
    if (!g_game_display) g_game_display = dpy;
    if (!g_game_window && w) g_game_window = w;

    int r = orig_XCheckTypedWindowEvent(dpy, w, type, ev);
    if (r == 0) return 0;
    if (x11_dispatch_event(ev)) {
        fill_noexpose(ev, dpy);
    }
    return 1;
}

static int hook_XWindowEvent(Display *dpy, Window w, long mask, XEventStorage *ev) {
    if (!orig_XWindowEvent)
        orig_XWindowEvent = (XWindowEvent_fn)hook_orig("XWindowEvent");
    if (!g_game_display) g_game_display = dpy;
    if (!g_game_window && w) g_game_window = w;

    int r = orig_XWindowEvent(dpy, w, mask, ev);
    if (r != 0) return r;
    if (x11_dispatch_event(ev)) {
        fill_noexpose(ev, dpy);
    }
    return 0;
}

/* ── Init / shutdown ─────────────────────────────────────────────────── */

/* XSelectInput hook — inject pointer event masks so we receive mouse events
 * even if the game didn't request them (e.g. glxgears only selects KeyPress).
 * We OR-in ButtonPressMask | ButtonReleaseMask | PointerMotionMask so that
 * ButtonPress/ButtonRelease/MotionNotify events flow into the X event queue
 * where our XNextEvent-family hooks can intercept them when captured. */
static int hook_XSelectInput(Display *dpy, Window w, long mask) {
    if (!orig_XSelectInput)
        orig_XSelectInput = (XSelectInput_fn)hook_orig("XSelectInput");
    if (!g_game_display) g_game_display = dpy;
    if (!g_game_window && w) g_game_window = w;

    /* Inject pointer + key release masks. KeyReleaseMask so we get
     * KeyRelease events (needed to stop webview key repeat timer). */
    mask |= ButtonPressMask | ButtonReleaseMask | PointerMotionMask | KeyReleaseMask;

    return orig_XSelectInput(dpy, w, mask);
}

int idk_x11_input_init(void) {
    if (g_hook_installed) return 0;

    if (resolve_x11_symbols() != 0) return -1;

    /* Reuse Wayland's configure_hotkey() — same env var (IDK_HOTKEY_CAPTURE),
     * same parsing, same scancode/keysym tables. The hotkey detection
     * (is_hotkey) is also shared. */
    configure_hotkey();

    /* Reuse Wayland's init_input_socket() — same socket path scheme
     * (/tmp/idk-overlay-<pid>-input), same accept thread, same
     * send_event_to_webview helper. The webview side is identical. */
    if (init_input_socket() != 0)
        XERR("input socket init failed — events will be dropped");

    /* Install syringe hooks for XNextEvent-family. */
    #define INSTALL(name) \
        syringe_hook_install(#name, (void *)hook_##name, (void **)&orig_##name)

    int n = 0;
    n += INSTALL(XNextEvent);
    n += INSTALL(XPeekEvent);
    n += INSTALL(XCheckWindowEvent);
    n += INSTALL(XMaskEvent);
    n += INSTALL(XCheckMaskEvent);
    n += INSTALL(XCheckTypedEvent);
    n += INSTALL(XCheckTypedWindowEvent);
    n += INSTALL(XWindowEvent);
    n += INSTALL(XSelectInput);

    #undef INSTALL

    if (n == 0) {
        XERR("no X11 hooks installed\n");
        return -1;
    }

    g_hook_installed = 1;
    XLOG("hooks installed: %d XNextEvent-family functions", n);
    return 0;
}

void idk_x11_input_shutdown(void) {
    if (!g_hook_installed) return;
    g_hook_installed = 0;

    /* Release cursor grab if active */
    if (g_cursor_grabbed && g_game_display && fn_XUngrabPointer) {
        fn_XUngrabPointer(g_game_display, 0);
        g_cursor_grabbed = 0;
    }
    if (g_blank_cursor && g_game_display && fn_XFreeCursor) {
        fn_XFreeCursor(g_game_display, g_blank_cursor);
        g_blank_cursor = 0;
    }

    /* Note: teardown_input_socket() is shared with Wayland. Only call
     * it once — Wayland's shutdown will handle it if both are active.
     * If only X11 was active, we call it here. */
    if (g_input_listen_fd >= 0) {
        teardown_input_socket();
    }

    if (g_x11_handle) {
        dlclose(g_x11_handle);
        g_x11_handle = NULL;
    }
}
