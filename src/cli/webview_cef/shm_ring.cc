#include "shm_ring.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
int create_memfd() {
  return static_cast<int>(syscall(SYS_memfd_create, "idk-shm-ring", MFD_CLOEXEC | MFD_ALLOW_SEALING));
}
}

ShmRing::~ShmRing() {
  Reset();
}

bool ShmRing::Init(uint64_t generation, uint32_t slot_count, size_t slot_capacity, size_t max_bytes) {
  Reset();
  if (generation == 0 || slot_count == 0 || slot_count > 3 || slot_capacity == 0 ||
      slot_capacity > max_bytes || slot_capacity > SIZE_MAX / slot_count)
    return false;
  const size_t bytes = slot_capacity * slot_count;
  fd_ = create_memfd();
  if (fd_ < 0)
    return false;
  if (ftruncate(fd_, static_cast<off_t>(bytes)) < 0 ||
      fcntl(fd_, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) < 0) {
    Reset();
    return false;
  }
  mapping_ = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
  if (mapping_ == MAP_FAILED) {
    mapping_ = nullptr;
    Reset();
    return false;
  }
  generation_ = generation;
  slot_capacity_ = slot_capacity;
  mapped_bytes_ = bytes;
  slots_.assign(slot_count, Slot{});
  return true;
}

int ShmRing::Acquire(uint64_t frame_id, size_t payload_bytes) {
  if (!mapping_ || frame_id == 0 || payload_bytes > slot_capacity_)
    return -1;
  for (size_t i = 0; i < slots_.size(); ++i) {
    if (!slots_[i].in_flight) {
      slots_[i].frame_id = frame_id;
      slots_[i].in_flight = true;
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool ShmRing::Write(int slot, size_t offset, const void *data, size_t bytes) {
  if (!data || slot < 0 || static_cast<size_t>(slot) >= slots_.size() || !slots_[slot].in_flight ||
      offset > slot_capacity_ || bytes > slot_capacity_ - offset)
    return false;
  std::memcpy(static_cast<unsigned char *>(mapping_) + static_cast<size_t>(slot) * slot_capacity_ + offset, data, bytes);
  return true;
}

bool ShmRing::Release(int slot, uint64_t generation, uint64_t frame_id) {
  if (generation != generation_ || slot < 0 || static_cast<size_t>(slot) >= slots_.size() ||
      !slots_[slot].in_flight || slots_[slot].frame_id != frame_id)
    return false;
  slots_[slot] = Slot{};
  return true;
}

void ShmRing::Reset() {
  if (mapping_) {
    munmap(mapping_, mapped_bytes_);
    mapping_ = nullptr;
  }
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
  generation_ = 0;
  slot_capacity_ = 0;
  mapped_bytes_ = 0;
  slots_.clear();
}

bool ShmRing::slot_in_flight(int slot) const {
  return slot >= 0 && static_cast<size_t>(slot) < slots_.size() && slots_[slot].in_flight;
}
