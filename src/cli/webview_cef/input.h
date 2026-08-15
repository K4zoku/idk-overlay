#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/transport.h"

class View;

/* Input socket reader. Own thread: connects to the game's input socket
 * (IDK_INPUT_ABSTRACT or <frame-socket>-input), translates idk_input_event_t
 * into CEF events and posts them to the UI thread. Also synthesizes key
 * repeat from the game's rate/delay info (timerfd in the poll set). */
class InputThread {
public:
  InputThread(View *view, const std::string &frame_socket);
  ~InputThread();

  void Start();
  void Stop();
  void QueueCursor(const idk_cursor_update_t &cursor, std::vector<uint8_t> pixels);

private:
  void Run();
  bool Connect();
  bool FlushCursor();
  void HandleEvent(const idk_input_event_t &ev);
  void ArmRepeat(uint32_t keycode, uint32_t keysym, uint16_t mods);
  void DisarmRepeat();
  void HandleRepeat();

  View *view_; /* UI-thread owned; only via PostToUI */
  std::string name_;
  std::thread thread_;
  std::atomic<bool> stop_{false};

  idk_transport_t tp_{};
  int watch_fd_ = -1; /* input fd or SHM wake eventfd */
  int wake_fd_ = -1;
  int command_fd_ = -1;
  std::mutex cursor_mutex_;
  idk_cursor_update_t cursor_{};
  std::vector<uint8_t> cursor_pixels_;
  bool cursor_pending_ = false;

  int repeat_fd_ = -1; /* timerfd */
  uint32_t repeat_rate_ = 25;
  uint32_t repeat_delay_ = 500;
  uint32_t repeat_keycode_ = 0;
  uint32_t repeat_keysym_ = 0;
  uint16_t repeat_mods_ = 0;
  bool repeat_armed_ = false;

  int mouse_x_ = 0;
  int mouse_y_ = 0;
  bool capture_ = false;
};
