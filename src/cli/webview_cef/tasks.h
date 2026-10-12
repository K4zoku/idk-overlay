#pragma once
#include <cstdint>
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
