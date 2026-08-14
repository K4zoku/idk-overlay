#include "input.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "core/log.h"
#include "include/cef_browser.h"
#include "keys.h"
#include "public/idk_input.h"
#include "tasks.h"
#include "view.h"

#define WHEEL_SCALE 12 /* match the Qt webview */
#define INPUT_RETRY_MS 2000
#define INPUT_MAX_RETRIES 15

InputThread::InputThread(View *view, const std::string &frame_socket) : view_(view) {
  const char *env = getenv("IDK_INPUT_ABSTRACT");
  if (env && *env) {
    name_ = std::string(1, '\0') + env; /* abstract */
  } else {
    name_ = frame_socket + "-input";
  }
}

InputThread::~InputThread() { Stop(); }

void InputThread::Start() { thread_ = std::thread(&InputThread::Run, this); }

void InputThread::Stop() {
  stop_.store(true);
  if (thread_.joinable())
    thread_.join();
}

bool InputThread::Connect() {
  if (tp_.ready || tp_._client_fd >= 0 || tp_._server_fd >= 0)
    idk_tp_destroy(&tp_);
  memset(&tp_, 0, sizeof(tp_));

  if (idk_tp_init(&tp_, IDK_TP_PRODUCER, name_.c_str()) != 0) {
    IDK_LOG("input-cef", "transport init failed for %s\n", name_.c_str());
    return false;
  }
  if (!tp_.ready) {
    idk_tp_destroy(&tp_);
    return false;
  }

  watch_fd_ = tp_._client_fd;
  if (tp_.backend == IDK_TP_SHM) {
    memcpy(&wake_fd_, tp_._rsv + 40, sizeof(wake_fd_));
    if (wake_fd_ <= 0) {
      IDK_LOG("input-cef", "SHM transport has no eventfd\n");
      idk_tp_destroy(&tp_);
      return false;
    }
    watch_fd_ = wake_fd_;
  }
  return true;
}

void InputThread::Run() {
  bool connected = false;
  int retries = 0;

  while (!stop_.load()) {
    if (!connected) {
      if (Connect()) {
        connected = true;
        retries = 0;
        IDK_LOG("input-cef", "input connected to %s%s\n", name_[0] == '\0' ? "\\0" : "",
                name_[0] == '\0' ? name_.c_str() + 1 : name_.c_str());
      } else {
        retries++;
        if (retries == INPUT_MAX_RETRIES)
          IDK_LOG("input-cef",
                  "giving up after %d tries - game may not be a "
                  "wayland client\n",
                  retries);
        struct pollfd pf = {.fd = -1, .events = 0, .revents = 0};
        poll(&pf, 0, INPUT_RETRY_MS); /* sleep, interruptible-ish */
        continue;
      }
    }

    struct pollfd fds[2];
    fds[0] = {.fd = watch_fd_, .events = POLLIN, .revents = 0};
    int nfds = 1;
    if (repeat_armed_) {
      fds[1] = {.fd = repeat_fd_, .events = POLLIN, .revents = 0};
      nfds = 2;
    }
    int rc = poll(fds, nfds, 100); /* heartbeat for stop */
    if (rc < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (rc == 0)
      continue;

    if (fds[0].revents & POLLIN) {
      if (wake_fd_ >= 0) {
        eventfd_t val;
        while (eventfd_read(wake_fd_, &val) == 0) {
        }
      }
      idk_input_event_t ev;
      while (!stop_.load()) {
        int r = idk_tp_recv_input(&tp_, &ev);
        if (r <= 0) {
          if (r < 0) {
            idk_tp_disconnect_client(&tp_);
            connected = false;
            IDK_LOG("input-cef", "input transport disconnected\n");
          }
          break;
        }
        HandleEvent(ev);
      }
    }

    if (repeat_armed_ && (fds[1].revents & POLLIN))
      HandleRepeat();
  }

  if (tp_.ready || tp_._client_fd >= 0 || tp_._server_fd >= 0)
    idk_tp_destroy(&tp_);
}

/* ── Event translation → UI thread ─────────────────────────────────── */

static CefKeyEvent MakeKey(cef_key_event_type_t type, uint32_t sym, uint32_t code, int mods) {
  CefKeyEvent ev;
  ev.type = type;
  ev.modifiers = mods;
  ev.windows_key_code = KeysymToVk(sym);
  ev.native_key_code = code;
  ev.is_system_key = 0;
  ev.character = 0;
  return ev;
}

void InputThread::HandleEvent(const idk_input_event_t &ev) {
  switch (ev.type) {
  case IDK_INPUT_KEY: {
    uint32_t sym = ev.u.key.keysym;
    uint32_t code = ev.u.key.keycode;
    int mods = IdkModsToCef(ev.mods);
    bool press = (ev.flags & IDK_INPUT_FLAG_PRESS) != 0;

    if (press) {
      if (code != 0 && capture_)
        ArmRepeat(code, sym, ev.mods);
      PostToUI([self = view_, k = MakeKey(KEYEVENT_RAWKEYDOWN, sym, code, mods)] { self->SendKeyEventUI(k); });
      /* CHAR for printable keys and Return (like the Qt backend). */
      if (sym >= 0x20 && sym < 0x7f) {
        CefKeyEvent ch = MakeKey(KEYEVENT_CHAR, sym, code, mods);
        ch.windows_key_code = (int)sym;
        ch.character = (char16_t)sym;
        PostToUI([self = view_, k = ch] { self->SendKeyEventUI(k); });
      } else if (sym == 0xff0d) {
        CefKeyEvent ch = MakeKey(KEYEVENT_CHAR, sym, code, mods);
        ch.windows_key_code = 0x0D;
        ch.character = 0x0D;
        PostToUI([self = view_, k = ch] { self->SendKeyEventUI(k); });
      }
    } else {
      if (code == repeat_keycode_)
        DisarmRepeat();
      PostToUI([self = view_, k = MakeKey(KEYEVENT_KEYUP, sym, code, mods)] { self->SendKeyEventUI(k); });
    }
    break;
  }

  case IDK_INPUT_BUTTON: {
    mouse_x_ = ev.u.btn.x;
    mouse_y_ = ev.u.btn.y;
    int btn = IdkButtonToCef(ev.u.btn.button);
    bool up = (ev.flags & IDK_INPUT_FLAG_PRESS) == 0;
    PostToUI([self = view_, x = mouse_x_, y = mouse_y_, m = IdkModsToCef(ev.mods), b = btn, u = up] {
      self->SendMouseClickUI(x, y, m, b, u);
    });
    break;
  }

  case IDK_INPUT_MOTION:
    mouse_x_ = ev.u.motion.x;
    mouse_y_ = ev.u.motion.y;
    PostToUI([self = view_, x = mouse_x_, y = mouse_y_, m = IdkModsToCef(ev.mods)] { self->SendMouseMoveUI(x, y, m); });
    break;

  case IDK_INPUT_AXIS: {
    int dx = -ev.u.axis.dx * WHEEL_SCALE;
    int dy = -ev.u.axis.dy * WHEEL_SCALE;
    PostToUI([self = view_, x = mouse_x_, y = mouse_y_, m = IdkModsToCef(ev.mods), dx, dy] {
      self->SendMouseWheelUI(x, y, m, dx, dy);
    });
    break;
  }

  case IDK_INPUT_STATE: {
    bool captured = (ev.flags & IDK_INPUT_FLAG_CAPTURE) != 0;
    if (captured != capture_) {
      capture_ = captured;
      if (!captured)
        DisarmRepeat();
      IDK_LOG("input-cef", "capture %s\n", captured ? "ENABLED" : "DISABLED");
      PostToUI([self = view_, c = captured] { self->SetCapture(c); });
    }
    break;
  }

  case IDK_INPUT_REPEAT:
    repeat_rate_ = ev.u.repeat.rate > 0 ? ev.u.repeat.rate : 25;
    repeat_delay_ = ev.u.repeat.delay > 0 ? ev.u.repeat.delay : 500;
    IDK_LOG("input-cef", "repeat info: rate=%u cps delay=%u ms\n", repeat_rate_, repeat_delay_);
    break;

  case IDK_INPUT_OVERLAY: {
    bool v = ev.u.overlay.visible != 0;
    IDK_LOG("input-cef", "overlay %s\n", v ? "SHOW" : "HIDE");
    PostToUI([self = view_, v] { self->SetOverlayVisible(v); });
    break;
  }
  }
}

/* ── Key repeat (timerfd on the poll set) ───────────────────────────── */

void InputThread::ArmRepeat(uint32_t keycode, uint32_t keysym, uint16_t mods) {
  if (repeat_fd_ < 0) {
    repeat_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (repeat_fd_ < 0)
      return;
  }
  repeat_keycode_ = keycode;
  repeat_keysym_ = keysym;
  repeat_mods_ = mods;
  repeat_armed_ = true;

  struct itimerspec its = {};
  its.it_value.tv_nsec = (long)repeat_delay_ * 1000000L; /* first: delay */
  its.it_interval.tv_nsec = 1000000000L / repeat_rate_;  /* then: rate */
  timerfd_settime(repeat_fd_, 0, &its, nullptr);
}

void InputThread::DisarmRepeat() {
  if (!repeat_armed_)
    return;
  repeat_armed_ = false;
  if (repeat_fd_ >= 0) {
    struct itimerspec its = {};
    timerfd_settime(repeat_fd_, 0, &its, nullptr);
  }
  repeat_keycode_ = 0;
}

void InputThread::HandleRepeat() {
  uint64_t expirations = 0;
  if (read(repeat_fd_, &expirations, sizeof(expirations)) < 0)
    return;
  if (!repeat_armed_)
    return;

  int mods = IdkModsToCef(repeat_mods_);
  CefKeyEvent raw = MakeKey(KEYEVENT_RAWKEYDOWN, repeat_keysym_, repeat_keycode_, mods);
  raw.modifiers |= EVENTFLAG_IS_REPEAT;
  PostToUI([self = view_, k = raw] { self->SendKeyEventUI(k); });

  if (repeat_keysym_ >= 0x20 && repeat_keysym_ < 0x7f) {
    CefKeyEvent ch = MakeKey(KEYEVENT_CHAR, repeat_keysym_, repeat_keycode_, mods);
    ch.windows_key_code = (int)repeat_keysym_;
    ch.character = (char16_t)repeat_keysym_;
    PostToUI([self = view_, k = ch] { self->SendKeyEventUI(k); });
  }
}
