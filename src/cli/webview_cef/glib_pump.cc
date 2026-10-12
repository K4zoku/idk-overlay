#include "glib_pump.h"

#include <dlfcn.h>

#include <algorithm>

namespace {
template <typename Fn> bool Symbol(void *lib, const char *name, Fn &out) {
  out = reinterpret_cast<Fn>(dlsym(lib, name));
  return out != nullptr;
}
} // namespace

GlibPump::GlibPump() {
  // Use the GLib instance already loaded by Chromium; never load a separate
  // copy with another default main context.
  lib_ = dlopen("libglib-2.0.so.0", RTLD_NOW | RTLD_NOLOAD);
  if (!lib_)
    return;
  ContextFn get_default = nullptr;
  if (!Symbol(lib_, "g_main_context_default", get_default) ||
      !Symbol(lib_, "g_main_context_acquire", acquire_) ||
      !Symbol(lib_, "g_main_context_release", release_) ||
      !Symbol(lib_, "g_main_context_prepare", prepare_) ||
      !Symbol(lib_, "g_main_context_query", query_) ||
      !Symbol(lib_, "g_main_context_check", check_) ||
      !Symbol(lib_, "g_main_context_dispatch", dispatch_)) {
    dlclose(lib_);
    lib_ = nullptr;
    return;
  }
  context_ = get_default();
}

GlibPump::~GlibPump() {
  if (acquired_)
    release_(context_);
  if (lib_)
    dlclose(lib_);
}

void GlibPump::Prepare(std::vector<pollfd> &fds, int &timeout_ms) {
  if (!context_ || !acquire_(context_))
    return;
  acquired_ = true;
  offset_ = fds.size();
  const bool prepared_ready = prepare_(context_, &priority_) != 0;
  int glib_timeout = -1;
  for (;;) {
    const int capacity = static_cast<int>(glib_fds_.size());
    const int count = query_(context_, priority_, &glib_timeout, glib_fds_.data(), capacity);
    if (count < 0) {
      glib_fds_.clear();
      break;
    }
    if (count <= capacity) {
      glib_fds_.resize(static_cast<size_t>(count));
      break;
    }
    glib_fds_.resize(static_cast<size_t>(count));
  }
  for (auto fd : glib_fds_) {
    fd.revents = 0;
    fds.push_back(fd);
  }
  if (prepared_ready)
    timeout_ms = 0;
  else if (glib_timeout > 0)
    timeout_ms = std::min(timeout_ms, glib_timeout);
  // Chromium's MessagePumpGlib may report an unready 0ms source while
  // external_message_pump=true. Honoring that timeout would busy-spin.
  // Real descriptor readiness and CEF's 33ms reference fallback still wake us.
}

void GlibPump::Dispatch(const std::vector<pollfd> &fds, bool poll_succeeded) {
  if (!acquired_)
    return;
  if (poll_succeeded) {
    for (size_t i = 0; i < glib_fds_.size(); ++i)
      glib_fds_[i].revents = fds[offset_ + i].revents;
    if (check_(context_, priority_, glib_fds_.data(), static_cast<int>(glib_fds_.size())))
      dispatch_(context_);
  }
  release_(context_);
  acquired_ = false;
}
