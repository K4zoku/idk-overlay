#pragma once
#include <cstdint>

/* keysym (X11) → Windows VK code. Returns 0 for unmapped keys. */
int KeysymToVk(uint32_t sym);

/* IDK_MOD_* bitmask → CEF_EVENTFLAG_* bitmask. */
int IdkModsToCef(uint16_t mods);

/* IDK mouse button (BTN_*) → cef_mouse_button_type_t value. -1 unknown. */
int IdkButtonToCef(uint32_t button);
