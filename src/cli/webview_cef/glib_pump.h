#pragma once

#include <poll.h>

#include <cstddef>
#include <vector>

// CEF's external message pump does not dispatch the GLib default main
// context on Linux. Fold its sources into the browser-process poll loop;
// all GLib context operations stay on the browser UI thread.
class GlibPump {
public:
  GlibPump();
  ~GlibPump();
  GlibPump(const GlibPump &) = delete;
  GlibPump &operator=(const GlibPump &) = delete;

  bool Available() const { return context_ != nullptr; }

  // Append GLib descriptors to the application's poll vector and tighten
  // timeout_ms only when GLib has real work or a positive timer deadline.
  void Prepare(std::vector<pollfd> &fds, int &timeout_ms);
  // Always call after Prepare(), even if poll() was interrupted, to release
  // the default GLib context acquired on the UI thread.
  void Dispatch(const std::vector<pollfd> &fds, bool poll_succeeded);

private:
  using ContextFn = void *(*)();
  using AcquireFn = int (*)(void *);
  using ReleaseFn = void (*)(void *);
  using PrepareFn = int (*)(void *, int *);
  using QueryFn = int (*)(void *, int, int *, pollfd *, int);
  using CheckFn = int (*)(void *, int, pollfd *, int);
  using DispatchFn = void (*)(void *);

  void *lib_ = nullptr;
  void *context_ = nullptr;
  AcquireFn acquire_ = nullptr;
  ReleaseFn release_ = nullptr;
  PrepareFn prepare_ = nullptr;
  QueryFn query_ = nullptr;
  CheckFn check_ = nullptr;
  DispatchFn dispatch_ = nullptr;

  std::vector<pollfd> glib_fds_;
  size_t offset_ = 0;
  int priority_ = 0;
  bool acquired_ = false;
};
