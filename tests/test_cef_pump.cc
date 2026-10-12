#include "../src/cli/webview_cef/glib_pump.h"
#include "../src/cli/webview_cef/pump.h"

#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <dlfcn.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <thread>
#include <vector>

static void TestScheduling() {
  CefPump pump;
  assert(pump.Valid());
  pump.Schedule(30000);
  pump.Reconcile();
  assert(!pump.TakeDue());
  pump.Schedule(0);
  pump.Reconcile();
  assert(pump.TakeDue());
  assert(!pump.TakeDue());
  pump.Schedule(0);
  assert(pump.TakeDue());
  pump.Schedule(-1);
  assert(pump.TakeDue());
  pump.Cancel();
  pump.Schedule(0);
  assert(!pump.TakeDue());
}

static void TestConcurrentScheduling() {
  CefPump pump;
  assert(pump.Valid());
  std::thread worker([&] {
    for (int i = 0; i < 3000; ++i)
      pump.Schedule(i % 2 ? 0 : 1000);
  });
  for (int i = 0; i < 3000; ++i) {
    pump.TakeDue();
    pump.Reconcile();
    pump.DrainWake();
  }
  worker.join();
  pump.Schedule(0);
  pump.Reconcile();
  assert(pump.TakeDue());
}

static int TimeoutCallback(void *data) {
  *static_cast<bool *>(data) = true;
  return 0; // remove source
}

static void TestGlibDispatch() {
  void *lib = dlopen("libglib-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);
  if (!lib)
    return; // GLib is optional in this stand-alone test.
  using AddTimeout = unsigned int (*)(unsigned int, int (*)(void *), void *);
  auto add_timeout = reinterpret_cast<AddTimeout>(dlsym(lib, "g_timeout_add"));
  assert(add_timeout);
  GlibPump glib;
  assert(glib.Available());
  bool fired = false;
  add_timeout(2, TimeoutCallback, &fired);
  for (int i = 0; i < 12 && !fired; ++i) {
    std::vector<pollfd> fds;
    int timeout = 33;
    glib.Prepare(fds, timeout);
    const int rc = poll(fds.data(), fds.size(), timeout);
    glib.Dispatch(fds, rc >= 0);
  }
  assert(fired);
  dlclose(lib);
}

int main() {
  TestScheduling();
  TestConcurrentScheduling();
  TestGlibDispatch();
}
