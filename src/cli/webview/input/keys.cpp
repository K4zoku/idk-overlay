#include "input_receiver.h"
#include "internal.h"

struct SymQtEntry {
  quint32 sym;
  int qtKey;
};

static const SymQtEntry SYM_QT[] = {{0xff08, Qt::Key_Backspace},
                                    {0xff09, Qt::Key_Tab},
                                    {0xff0d, Qt::Key_Return},
                                    {0xff1b, Qt::Key_Escape},
                                    {0xff50, Qt::Key_Home},
                                    {0xff51, Qt::Key_Left},
                                    {0xff52, Qt::Key_Up},
                                    {0xff53, Qt::Key_Right},
                                    {0xff54, Qt::Key_Down},
                                    {0xff55, Qt::Key_PageUp},
                                    {0xff56, Qt::Key_PageDown},
                                    {0xff57, Qt::Key_End},
                                    {0xff63, Qt::Key_Insert},
                                    {0xffff, Qt::Key_Delete},
                                    {0xffbe, Qt::Key_F1},
                                    {0xffbf, Qt::Key_F2},
                                    {0xffc0, Qt::Key_F3},
                                    {0xffc1, Qt::Key_F4},
                                    {0xffc2, Qt::Key_F5},
                                    {0xffc3, Qt::Key_F6},
                                    {0xffc4, Qt::Key_F7},
                                    {0xffc5, Qt::Key_F8},
                                    {0xffc6, Qt::Key_F9},
                                    {0xffc7, Qt::Key_F10},
                                    {0xffc8, Qt::Key_F11},
                                    {0xffc9, Qt::Key_F12},
                                    {0xffe1, Qt::Key_Shift},
                                    {0xffe2, Qt::Key_Shift},
                                    {0xffe3, Qt::Key_Control},
                                    {0xffe4, Qt::Key_Control},
                                    {0xffe5, Qt::Key_CapsLock},
                                    {0xffe7, Qt::Key_Meta},
                                    {0xffe8, Qt::Key_Meta},
                                    {0xffe9, Qt::Key_Alt},
                                    {0xffea, Qt::Key_Alt},
                                    {0xffeb, Qt::Key_Meta},
                                    {0xffec, Qt::Key_Meta},
                                    {0xff8d, Qt::Key_Enter},
                                    {0xff95, Qt::Key_Home},
                                    {0xff96, Qt::Key_Left},
                                    {0xff97, Qt::Key_Up},
                                    {0xff98, Qt::Key_Right},
                                    {0xff99, Qt::Key_Down},
                                    {0xff9a, Qt::Key_PageUp},
                                    {0xff9b, Qt::Key_PageDown},
                                    {0xff9c, Qt::Key_End},
                                    {0xff9e, Qt::Key_Insert},
                                    {0xff9f, Qt::Key_Delete},
                                    {0, 0}};

int keysymToQtKey(quint32 sym) {
  if (sym < 0x20)
    return 0;
  if (sym >= 0x20 && sym < 0x7f) {
    char c = (char)sym;
    if (c >= 'a' && c <= 'z')
      c -= 32;
    return (int)c;
  }
  for (int i = 0; SYM_QT[i].sym; i++) {
    if (SYM_QT[i].sym == sym)
      return SYM_QT[i].qtKey;
  }
  if (sym >= 0xffbe && sym <= 0xffc9)
    return Qt::Key_F1 + (sym - 0xffbe);
  return 0;
}

Qt::KeyboardModifiers idkModsToQt(quint32 mods) {
  Qt::KeyboardModifiers m = Qt::NoModifier;
  if (mods & IDK_MOD_CTRL)
    m |= Qt::ControlModifier;
  if (mods & IDK_MOD_SHIFT)
    m |= Qt::ShiftModifier;
  if (mods & IDK_MOD_ALT)
    m |= Qt::AltModifier;
  if (mods & IDK_MOD_SUPER)
    m |= Qt::MetaModifier;
  return m;
}

quint64 nowMs() { return (quint64)QDateTime::currentMSecsSinceEpoch(); }
