#ifndef IDK_WL_XKB_H
#define IDK_WL_XKB_H

#include <stdint.h>

struct xkb_context;
struct xkb_keymap;
struct xkb_state;

/* xkbcommon function table — X-macro pattern */
#define XKB_FOREACH(F)                                                                                                 \
  F(struct xkb_context *, xkb_context_new, (int flags))                                                                \
  F(void, xkb_context_unref, (struct xkb_context *))                                                                   \
  F(struct xkb_keymap *, xkb_keymap_new_from_string, (struct xkb_context *, char *, int, int))                         \
  F(void, xkb_keymap_unref, (struct xkb_keymap *))                                                                     \
  F(struct xkb_state *, xkb_state_new, (struct xkb_keymap *))                                                          \
  F(void, xkb_state_unref, (struct xkb_state *))                                                                       \
  F(int, xkb_state_update_key, (struct xkb_state *, uint32_t, int))                                                    \
  F(uint32_t, xkb_state_key_get_one_sym, (struct xkb_state *, uint32_t))                                               \
  F(int, xkb_state_update_mask, (struct xkb_state *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))      \
  F(uint32_t, xkb_state_serialize_mods, (struct xkb_state *, int))                                                     \
  F(int, xkb_state_mod_index_is_active, (struct xkb_state *, uint32_t, int))                                           \
  F(uint32_t, xkb_keymap_mod_get_index, (struct xkb_keymap *, const char *))                                           \
  F(uint32_t, xkb_keysym_from_name, (const char *, int))

#define XKB_TYPEDEF(ret, name, params) typedef ret(*name##_fn) params;
XKB_FOREACH(XKB_TYPEDEF)
#undef XKB_TYPEDEF

/* Resolved xkbcommon function pointers */
#define XKB_EXTERN(ret, name, params) extern name##_fn fn_##name;
XKB_FOREACH(XKB_EXTERN)
#undef XKB_EXTERN

extern void *g_xkb_handle;
extern struct xkb_context *g_xkb_ctx;
extern struct xkb_keymap *g_xkb_keymap;
extern struct xkb_state *g_xkb_state;

extern uint32_t g_mod_idx_ctrl;
extern uint32_t g_mod_idx_shift;
extern uint32_t g_mod_idx_alt;
extern uint32_t g_mod_idx_super;

/* xkbcommon constants */
#define IDK_XKB_CONTEXT_NO_FLAGS 0
#define IDK_XKB_KEYMAP_FORMAT_TEXT_V1 1
#define IDK_XKB_KEYMAP_COMPILE_NO_FLAGS 0
#define IDK_XKB_KEY_DOWN 1
#define IDK_XKB_KEY_UP 2
#define IDK_XKB_STATE_MODS_DEPRESSED (1 << 0)
#define IDK_XKB_STATE_MODS_LATCHED (1 << 1)
#define IDK_XKB_STATE_MODS_LOCKED (1 << 2)
#define IDK_XKB_STATE_MODS_EFFECTIVE (1 << 3)
#define IDK_XKB_KEYSYM_NO_FLAGS 0

/* Hotkey keysyms */
#define IDK_XKB_KEY_Tab 0xff09
#define IDK_XKB_KEY_F1 0xffbe
#define IDK_XKB_KEY_F2 0xffbf
#define IDK_XKB_KEY_F3 0xffc0
#define IDK_XKB_KEY_F4 0xffc1
#define IDK_XKB_KEY_F5 0xffc2
#define IDK_XKB_KEY_F6 0xffc3
#define IDK_XKB_KEY_F7 0xffc4
#define IDK_XKB_KEY_F8 0xffc5
#define IDK_XKB_KEY_F9 0xffc6
#define IDK_XKB_KEY_F10 0xffc7
#define IDK_XKB_KEY_F11 0xffc8
#define IDK_XKB_KEY_F12 0xffc9
#define IDK_XKB_KEY_Scroll_Lock 0xff14
#define IDK_XKB_KEY_Pause 0xff13

#endif /* IDK_WL_XKB_H */
