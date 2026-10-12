#include "pump.h"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

CefPump::CefPump() {
  wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (!Valid()) {
    if (wake_fd_ >= 0)
      close(wake_fd_);
    if (timer_fd_ >= 0)
      close(timer_fd_);
    wake_fd_ = timer_fd_ = -1;
  }
}

CefPump::~CefPump() {
  Cancel();
  if (wake_fd_ >= 0)
    close(wake_fd_);
  if (timer_fd_ >= 0)
    close(timer_fd_);
}

int64_t CefPump::NowNs() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

void CefPump::Wake() {
  if (wake_fd_ < 0)
    return;
  uint64_t one = 1;
  if (write(wake_fd_, &one, sizeof(one)) < 0 && errno != EAGAIN)
    return;
}

void CefPump::Schedule(int64_t delay_ms) {
  // CEF defines delay_ms <= 0 as immediate work, not cancellation.
  constexpr int64_t kNsPerMs = 1000000;
  const int64_t now = NowNs();
  const int64_t positive_ms = delay_ms > 0 ? delay_ms : 0;
  const int64_t delay_ns = positive_ms > INT64_MAX / kNsPerMs ? INT64_MAX : positive_ms * kNsPerMs;
  const int64_t deadline = delay_ns > INT64_MAX - now ? INT64_MAX : now + delay_ns;
  {
    std::lock_guard<std::mutex> guard(lock_);
    if (stopped_)
      return;
    deadline_ns_ = deadline;
    ++generation_;
  }
  Wake();
}

void CefPump::Cancel() {
  {
    std::lock_guard<std::mutex> guard(lock_);
    stopped_ = true;
    deadline_ns_ = -1;
    ++generation_;
  }
  Reconcile();
}

void CefPump::DrainWake() {
  if (wake_fd_ < 0)
    return;
  uint64_t value;
  while (read(wake_fd_, &value, sizeof(value)) == sizeof(value))
    continue;
}

void CefPump::DrainTimer() {
  if (timer_fd_ < 0)
    return;
  uint64_t expirations;
  while (read(timer_fd_, &expirations, sizeof(expirations)) == sizeof(expirations))
    continue;
}

void CefPump::Reconcile() {
  if (timer_fd_ < 0)
    return;
  std::lock_guard<std::mutex> guard(lock_);
  if (generation_ == applied_generation_)
    return;

  itimerspec spec = {};
  if (deadline_ns_ >= 0) {
    spec.it_value.tv_sec = deadline_ns_ / 1000000000;
    spec.it_value.tv_nsec = deadline_ns_ % 1000000000;
  }
  if (timerfd_settime(timer_fd_, TFD_TIMER_ABSTIME, &spec, nullptr) == 0)
    applied_generation_ = generation_;
}

bool CefPump::TakeDue() {
  {
    std::lock_guard<std::mutex> guard(lock_);
    if (deadline_ns_ < 0 || deadline_ns_ > NowNs())
      return false;
    deadline_ns_ = -1;
    ++generation_;
  }
  Reconcile();
  return true;
}
