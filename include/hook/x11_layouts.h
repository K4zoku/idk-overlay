#ifndef IDK_X11_LAYOUTS_H
#define IDK_X11_LAYOUTS_H

#include <stdbool.h>
#include <stdint.h>

/* X11 opaque types */
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
#define KeyPress 2
#define KeyRelease 3
#define ButtonPress 4
#define ButtonRelease 5
#define MotionNotify 6
#define EnterNotify 7
#define LeaveNotify 8
#define FocusIn 9
#define FocusOut 10
#define KeymapNotify 11
#define Expose 12
#define GraphicsExpose 13
#define NoExpose 14
#define VisibilityNotify 15
#define CreateNotify 16
#define DestroyNotify 17
#define UnmapNotify 18
#define MapNotify 19
#define MapRequest 20
#define ReparentNotify 21
#define ConfigureNotify 22
#define ConfigureRequest 23
#define GravityNotify 24
#define ResizeRequest 25
#define CirculateNotify 26
#define CirculateRequest 27
#define PropertyNotify 28
#define SelectionClear 29
#define SelectionRequest 30
#define SelectionNotify 31
#define ColormapNotify 32
#define ClientMessage 33
#define MappingNotify 34
#define DestroyNotify2 35 /* not real, placeholder */
#define LASTEvent 36

/* X modifier masks (from X.h) */
#define ShiftMask (1 << 0)
#define LockMask (1 << 1)
#define ControlMask (1 << 2)
#define Mod1Mask (1 << 3) /* Alt */
#define Mod2Mask (1 << 4) /* NumLock */
#define Mod3Mask (1 << 5)
#define Mod4Mask (1 << 6) /* Super */
#define Mod5Mask (1 << 7) /* AltGr */

/* X button numbers */
#define Button1 1
#define Button2 2
#define Button3 3
#define Button4 4 /* wheel up */
#define Button5 5 /* wheel down */
#define Button6 6 /* wheel left */
#define Button7 7 /* wheel right */
#define Button8 8 /* back */
#define Button9 9 /* forward */

/* X event masks (from X.h) - used by XGrabPointer, XSelectInput */
#define NoEventMask (0L)
#define KeyPressMask (1L << 0)
#define KeyReleaseMask (1L << 1)
#define ButtonPressMask (1L << 2)
#define ButtonReleaseMask (1L << 3)
#define EnterWindowMask (1L << 4)
#define LeaveWindowMask (1L << 5)
#define PointerMotionMask (1L << 6)
#define PointerMotionHintMask (1L << 7)
#define Button1MotionMask (1L << 8)
#define Button2MotionMask (1L << 9)
#define Button3MotionMask (1L << 10)
#define Button4MotionMask (1L << 11)
#define Button5MotionMask (1L << 12)
#define ButtonMotionMask (1L << 13)
#define KeymapStateMask (1L << 14)
#define ExposureMask (1L << 15)
#define VisibilityChangeMask (1L << 16)
#define StructureNotifyMask (1L << 17)
#define ResizeRedirectMask (1L << 18)
#define SubstructureNotifyMask (1L << 19)
#define SubstructureRedirectMask (1L << 20)
#define FocusChangeMask (1L << 21)
#define PropertyChangeMask (1L << 22)
#define ColormapChangeMask (1L << 23)
#define OwnerGrabButtonMask (1L << 24)

/* X event struct layouts (from Xlibint.h) - byte-identical to Xlib */
typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Window window;
  Window root;
  Window subwindow;
  Time time;
  int x, y;
  int x_root, y_root;
  unsigned int state;
  unsigned int keycode;
  unsigned int same_screen;
} XKeyEventLayout;

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Window window;
  Window root;
  Window subwindow;
  Time time;
  int x, y;
  int x_root, y_root;
  unsigned int state;
  unsigned int button;
  unsigned int same_screen;
} XButtonEventLayout;

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Window window;
  Window root;
  Window subwindow;
  Time time;
  int x, y;
  int x_root, y_root;
  unsigned int state;
  char is_hint;
  unsigned int same_screen;
} XMotionEventLayout;

#endif /* IDK_X11_LAYOUTS_H */
