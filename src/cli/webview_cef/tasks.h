#pragma once
#include <cstdint>
#include <ctime>
#include <functional>
#include <utility>

#include "include/cef_task.h"

/* Run a std::function on the CEF UI thread. */
class FnTask : public CefTask {
public:
  explicit FnTask(std::function<void()> fn) : fn_(std::move(fn)) {}
  void Execute() override { fn_(); }

private:
  std::function<void()> fn_;
  IMPLEMENT_REFCOUNTING(FnTask);
};

template <typename Fn> void PostToUI(Fn &&fn) { CefPostTask(TID_UI, new FnTask(std::forward<Fn>(fn))); }

template <typename Fn> void PostToUIDelayed(Fn &&fn, int64_t delay_ms) {
  CefPostDelayedTask(TID_UI, new FnTask(std::forward<Fn>(fn)), delay_ms);
}

/* Monotonic ms, truncated to 31 bits (matches Qt's ACK timeout math). */
inline int now_ms() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int)((ts.tv_sec * 1000 + ts.tv_nsec / 1000000) & 0x7FFFFFFF);
}
