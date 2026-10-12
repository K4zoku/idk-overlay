#pragma once

#include <atomic>
#include <cstdint>

class CefPump {
public:
  CefPump();
  ~CefPump();
  CefPump(const CefPump &) = delete;
  CefPump &operator=(const CefPump &) = delete;

  bool Valid() const { return wake_fd_ >= 0 && timer_fd_ >= 0; }
  int WakeFd() const { return wake_fd_; }
  int TimerFd() const { return timer_fd_; }
  void Schedule(int64_t delay_ms);
  void Cancel();
  void Reconcile();
  void DrainWake();
  void DrainTimer();
  bool TakeDue();

private:
  static int64_t NowNs();
  void Wake();

  int wake_fd_ = -1;
  int timer_fd_ = -1;
  std::atomic<int64_t> deadline_ns_{-1};
  std::atomic<uint64_t> generation_{0};
  uint64_t applied_generation_ = 0;
  bool stopped_ = false;
};
