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
  if (stopped_)
    return;
  int64_t deadline = -1;
  if (delay_ms >= 0) {
    constexpr int64_t kNsPerMs = 1000000;
    const int64_t now = NowNs();
    const int64_t delay_ns = delay_ms > INT64_MAX / kNsPerMs ? INT64_MAX : delay_ms * kNsPerMs;
    deadline = delay_ns > INT64_MAX - now ? INT64_MAX : now + delay_ns;
  }
  deadline_ns_.store(deadline, std::memory_order_release);
  generation_.fetch_add(1, std::memory_order_acq_rel);
  Wake();
}

void CefPump::Cancel() {
  stopped_ = true;
  deadline_ns_.store(-1, std::memory_order_release);
  generation_.fetch_add(1, std::memory_order_acq_rel);
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
  const uint64_t generation = generation_.load(std::memory_order_acquire);
  if (generation == applied_generation_)
    return;

  const int64_t deadline = deadline_ns_.load(std::memory_order_acquire);
  itimerspec spec = {};
  if (deadline >= 0) {
    spec.it_value.tv_sec = deadline / 1000000000;
    spec.it_value.tv_nsec = deadline % 1000000000;
  }
  timerfd_settime(timer_fd_, TFD_TIMER_ABSTIME, &spec, nullptr);
  applied_generation_ = generation;
}

bool CefPump::TakeDue() {
  const int64_t deadline = deadline_ns_.load(std::memory_order_acquire);
  if (deadline < 0 || deadline > NowNs())
    return false;
  deadline_ns_.store(-1, std::memory_order_release);
  generation_.fetch_add(1, std::memory_order_acq_rel);
  Reconcile();
  return true;
}
