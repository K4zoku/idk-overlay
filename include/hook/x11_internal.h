#ifndef IDK_X11_INTERNAL_H
#define IDK_X11_INTERNAL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "hook/x11_input.h"
#include "hook/hook_util.h"
#include "hook/keycodes.h"
#include "public/idk_ipc.h"
#include "core/log.h"

/* Logging */
#define XLOG(fmt, ...) IDK_LOG("x11-input", fmt "\n", ##__VA_ARGS__)
#define XERR(fmt, ...) IDK_ERR("x11-input", fmt "\n", ##__VA_ARGS__)

/* X11 opaque types (avoid including <X11/Xlib.h>) */
typedef struct _XDisplay Display;
typedef unsigned long Window;
typedef unsigned long XID;
typedef unsigned long Atom;
typedef unsigned long Time;
typedef unsigned long KeyCode;
typedef unsigned long KeySym;
typedef unsigned long Cursor;

/* X11 Bool is int (Xlib defines it that way) */
#ifndef False
#define False 0
#endif
#ifndef True
#define True 1
#endif
#ifndef Bool
typedef int Bool;
#endif

/* XEvent generic header - first 5 fields are common to all X event types */
typedef struct {
    int type;
    unsigned long serial;
    Bool send_event;
    Display *display;
    Window window;
} XAnyEventHeaders;

/* Minimum XEvent struct - Xlib defines it as a union of all event types,
 * with a minimum size of 192 bytes on amd64. We declare a byte buffer
 * large enough to hold any XEvent, and cast as needed. */
#define X11_EVENT_BUFFER_SIZE 192
typedef union {
    XAnyEventHeaders xany;
    uint8_t raw[X11_EVENT_BUFFER_SIZE];
} XEventStorage;

/* X event type constants (from X.h) */
#define KeyPress        2
#define KeyRelease      3
#define ButtonPress     4
#define ButtonRelease   5
#define MotionNotify    6
#define EnterNotify     7
#define LeaveNotify     8
#define FocusIn         9
#define FocusOut        10
#define KeymapNotify    11
#define Expose          12
#define GraphicsExpose  13
#define NoExpose        14
#define VisibilityNotify 15
#define CreateNotify    16
#define DestroyNotify   17
#define UnmapNotify     18
#define MapNotify       19
#define MapRequest      20
#define ReparentNotify  21
#define ConfigureNotify 22
#define ConfigureRequest 23
#define GravityNotify   24
#define ResizeRequest   25
#define CirculateNotify 26
#define CirculateRequest 27
#define PropertyNotify  28
#define SelectionClear  29
#define SelectionRequest 30
#define SelectionNotify 31
#define ColormapNotify  32
#define ClientMessage   33
#define MappingNotify   34
#define DestroyNotify2  35  /* not real, placeholder */
#define LASTEvent       36

/* X modifier masks (from X.h) */
#define ShiftMask       (1<<0)
#define LockMask        (1<<1)
#define ControlMask     (1<<2)
#define Mod1Mask        (1<<3)   /* Alt */
#define Mod2Mask        (1<<4)   /* NumLock */
#define Mod3Mask        (1<<5)
#define Mod4Mask        (1<<6)   /* Super */
#define Mod5Mask        (1<<7)   /* AltGr */

/* X button numbers */
#define Button1         1
#define Button2         2
#define Button3         3
#define Button4         4   /* wheel up */
#define Button5         5   /* wheel down */
#define Button6         6   /* wheel left */
#define Button7         7   /* wheel right */
#define Button8         8   /* back */
#define Button9         9   /* forward */

/* X event masks (from X.h) - used by XGrabPointer, XSelectInput */
#define NoEventMask              (0L)
#define KeyPressMask             (1L<<0)
#define KeyReleaseMask           (1L<<1)
#define ButtonPressMask          (1L<<2)
#define ButtonReleaseMask        (1L<<3)
#define EnterWindowMask          (1L<<4)
#define LeaveWindowMask          (1L<<5)
#define PointerMotionMask        (1L<<6)
#define PointerMotionHintMask    (1L<<7)
#define Button1MotionMask        (1L<<8)
#define Button2MotionMask        (1L<<9)
#define Button3MotionMask        (1L<<10)
#define Button4MotionMask        (1L<<11)
#define Button5MotionMask        (1L<<12)
#define ButtonMotionMask         (1L<<13)
#define KeymapStateMask          (1L<<14)
#define ExposureMask             (1L<<15)
#define VisibilityChangeMask     (1L<<16)
#define StructureNotifyMask      (1L<<17)
#define ResizeRedirectMask       (1L<<18)
#define SubstructureNotifyMask   (1L<<19)
#define SubstructureRedirectMask (1L<<20)
#define FocusChangeMask          (1L<<21)
#define PropertyChangeMask       (1L<<22)
#define ColormapChangeMask       (1L<<23)
#define OwnerGrabButtonMask      (1L<<24)

/* X11 function pointers — X-macro pattern */

#define X11_EVENT_FOREACH(F) \
    F(int, XNextEvent,               (Display *, XEventStorage *)) \
    F(int, XPeekEvent,               (Display *, XEventStorage *)) \
    F(int, XCheckWindowEvent,        (Display *, Window, long, XEventStorage *)) \
    F(int, XMaskEvent,               (Display *, long, XEventStorage *)) \
    F(int, XCheckMaskEvent,          (Display *, long, XEventStorage *)) \
    F(int, XCheckTypedEvent,         (Display *, int, XEventStorage *)) \
    F(int, XCheckTypedWindowEvent,   (Display *, Window, int, XEventStorage *)) \
    F(int, XWindowEvent,             (Display *, Window, long, XEventStorage *)) \
    F(int, XPending,                 (Display *)) \
    F(int, XEventsQueued,            (Display *, int)) \
    F(int, XSelectInput,             (Display *, Window, long))

#define X11_CURSOR_FOREACH(F) \
    F(int,   XGetWindowAttributes,   (Display *, Window, void *)) \
    F(Cursor, XCreatePixmapCursor,   (Display *, void *, void *, void *, void *, unsigned int, unsigned int)) \
    F(int,   XFreePixmap,            (Display *, void *)) \
    F(void,  XDefineCursor,          (Display *, Window, Cursor)) \
    F(int,   XFreeCursor,            (Display *, Cursor)) \
    F(int,   XGrabPointer,           (Display *, Window, Bool, unsigned int, int, int, Window, Cursor, Time)) \
    F(int,   XUngrabPointer,         (Display *, Time)) \
    F(KeySym, XKeycodeToKeysym,      (Display *, KeyCode, int)) \
    F(KeySym, XStringToKeysym,       (const char *)) \
    F(int,   XFlush,                 (Display *)) \
    F(int,   XSync,                  (Display *, Bool))

#define X11_TYPEDEF(ret, name, params)    typedef ret (*name##_fn) params;
X11_EVENT_FOREACH(X11_TYPEDEF)
X11_CURSOR_FOREACH(X11_TYPEDEF)
#undef X11_TYPEDEF

#define X11_EXTERN_ORIG(ret, name, params) extern name##_fn orig_##name;
#define X11_EXTERN_FN(ret, name, params)   extern name##_fn fn_##name;
X11_EVENT_FOREACH(X11_EXTERN_ORIG)
X11_CURSOR_FOREACH(X11_EXTERN_FN)
#undef X11_EXTERN_ORIG
#undef X11_EXTERN_FN

/* Shared globals (mirror wayland_internal.h names where applicable) */
extern _Atomic int g_captured;
extern _Atomic int g_hotkey_pressed;
extern uint32_t g_hotkey_keysym;
extern uint32_t g_hotkey_scancode;
extern uint32_t g_hotkey_mods;
extern uint32_t g_mods;
extern int32_t g_repeat_rate;
extern int32_t g_repeat_delay;

/* Overlay visibility + overlay hotkey (defined in overlay.c) */
extern _Atomic int g_overlay_visible;
extern uint32_t g_hotkey_overlay_keysym;
extern uint32_t g_hotkey_overlay_scancode;
extern uint32_t g_hotkey_overlay_mods;

/* X11-specific globals */
extern void *g_x11_handle;          /* dlopen handle for libX11.so.6 */
extern int g_hook_installed;
extern Display *g_game_display;     /* cached display from first XNextEvent */
extern Window  g_game_window;       /* cached window from first X event */
extern Cursor  g_blank_cursor;      /* 1x1 transparent cursor for capture */
extern Cursor  g_saved_cursor;      /* cursor to restore on release */
extern int     g_cursor_grabbed;    /* XGrabPointer active */

/* Input socket (shared with wayland_socket.c structure) */
extern int g_input_listen_fd;
extern int g_client_fd;
extern int g_accept_thread_started;

int  init_input_socket(void);
void teardown_input_socket(void);
void send_event_to_webview(const idk_input_event_t *ev);
void send_capture_state(uint32_t capture);
void send_overlay_state(uint8_t visible);
void send_repeat_info(void);

/* Hotkey */
void configure_hotkey(void);
int  is_capture_hotkey(uint32_t key, uint32_t keysym);
int  is_overlay_hotkey(uint32_t key, uint32_t keysym);

/* Event dispatch (called from each XNextEvent-family hook) */
/* Returns 1 if the event should be swallowed (captured/hotkey),
 * 0 if it should be returned to the caller. */
int x11_dispatch_event(XEventStorage *ev);

#endif /* IDK_X11_INTERNAL_H */
