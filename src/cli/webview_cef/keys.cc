#include "keys.h"

#include <cctype>

#include "include/internal/cef_types.h"
#include "public/idk_input.h"

struct SymVk {
  uint32_t sym;
  int vk;
};

/* X11 keysym → Windows VK. Mirrors the Qt webview's keysym table, using
 * VK codes that Chromium's OSR path understands. */
static const SymVk SYM_VK[] = {
    {0xff08, 0x08}, /* BackSpace  VK_BACK    */
    {0xff09, 0x09}, /* Tab        VK_TAB     */
    {0xff0d, 0x0D}, /* Return     VK_RETURN  */
    {0xff1b, 0x1B}, /* Escape     VK_ESCAPE  */
    {0xff50, 0x24}, /* Home       VK_HOME    */
    {0xff51, 0x25}, /* Left       VK_LEFT    */
    {0xff52, 0x26}, /* Up         VK_UP      */
    {0xff53, 0x27}, /* Right      VK_RIGHT   */
    {0xff54, 0x28}, /* Down       VK_DOWN    */
    {0xff55, 0x21}, /* PageUp     VK_PRIOR   */
    {0xff56, 0x22}, /* PageDown   VK_NEXT    */
    {0xff57, 0x23}, /* End        VK_END     */
    {0xff63, 0x2D}, /* Insert     VK_INSERT  */
    {0xffff, 0x2E}, /* Delete     VK_DELETE  */
    {0xffe1, 0x10}, /* Shift_L    VK_SHIFT   */
    {0xffe2, 0x10}, /* Shift_R    VK_SHIFT   */
    {0xffe3, 0x11}, /* Control_L  VK_CONTROL */
    {0xffe4, 0x11}, /* Control_R  VK_CONTROL */
    {0xffe5, 0x14}, /* CapsLock   VK_CAPITAL */
    {0xffe7, 0x5B}, /* Meta_L     VK_LWIN    */
    {0xffe8, 0x5C}, /* Meta_R     VK_RWIN    */
    {0xffe9, 0x12}, /* Alt_L      VK_MENU    */
    {0xffea, 0x12}, /* Alt_R      VK_MENU    */
    {0xffeb, 0x5B}, /* Super_L    VK_LWIN    */
    {0xffec, 0x5C}, /* Super_R    VK_RWIN    */
    {0xff8d, 0x0D}, /* KP_Enter   VK_RETURN  */
    {0xff95, 0x24}, /* KP_Home    VK_HOME    */
    {0xff96, 0x25}, /* KP_Left    VK_LEFT    */
    {0xff97, 0x26}, /* KP_Up      VK_UP      */
    {0xff98, 0x27}, /* KP_Right   VK_RIGHT   */
    {0xff99, 0x28}, /* KP_Down    VK_DOWN    */
    {0xff9a, 0x21}, /* KP_PageUp  VK_PRIOR   */
    {0xff9b, 0x22}, /* KP_PageDown VK_NEXT   */
    {0xff9c, 0x23}, /* KP_End     VK_END     */
    {0xff9e, 0x2D}, /* KP_Insert  VK_INSERT  */
    {0xff9f, 0x2E}, /* KP_Delete  VK_DELETE  */
    {0, 0},
};

int KeysymToVk(uint32_t sym) {
  if (sym >= 0x20 && sym < 0x7f) {
    char c = (char)sym;
    if (c >= 'a' && c <= 'z')
      c -= 32; /* VK letters are uppercase */
    return (int)c;
  }
  for (int i = 0; SYM_VK[i].sym; i++)
    if (SYM_VK[i].sym == sym)
      return SYM_VK[i].vk;
  if (sym >= 0xffbe && sym <= 0xffc9)
    return 0x70 + (sym - 0xffbe); /* F1..F12 → VK_F1.. */
  return 0;
}

int IdkModsToCef(uint16_t mods) {
  int m = EVENTFLAG_NONE;
  if (mods & IDK_MOD_CTRL)
    m |= EVENTFLAG_CONTROL_DOWN;
  if (mods & IDK_MOD_SHIFT)
    m |= EVENTFLAG_SHIFT_DOWN;
  if (mods & IDK_MOD_ALT)
    m |= EVENTFLAG_ALT_DOWN;
  if (mods & IDK_MOD_SUPER)
    m |= EVENTFLAG_COMMAND_DOWN;
  return m;
}

int IdkButtonToCef(uint32_t button) {
  switch (button) {
  case 0x110: /* BTN_LEFT   */
    return 0; /* MBT_LEFT   */
  case 0x111: /* BTN_RIGHT  */
    return 2; /* MBT_RIGHT  */
  case 0x112: /* BTN_MIDDLE */
    return 1; /* MBT_MIDDLE */
  case 0x113: /* BTN_SIDE   */
    return 3; /* MBT_BACK   */
  case 0x114: /* BTN_EXTRA  */
    return 4; /* MBT_FORWARD */
  default:
    return -1;
  }
}
