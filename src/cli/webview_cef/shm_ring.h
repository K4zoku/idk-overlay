#ifndef IDK_WEBVIEW_CEF_SHM_RING_H
#define IDK_WEBVIEW_CEF_SHM_RING_H

#include <cstddef>
#include <cstdint>
#include <vector>

class ShmRing {
 public:
  ShmRing() = default;
  ~ShmRing();

  ShmRing(const ShmRing &) = delete;
  ShmRing &operator=(const ShmRing &) = delete;

  bool Init(uint64_t generation, uint32_t slot_count, size_t slot_capacity, size_t max_bytes);
  int Acquire(uint64_t frame_id, size_t payload_bytes);
  bool Write(int slot, size_t offset, const void *data, size_t bytes);
  bool Release(int slot, uint64_t generation, uint64_t frame_id);
  void Reset();

  int fd() const { return fd_; }
  uint64_t generation() const { return generation_; }
  uint32_t slot_count() const { return static_cast<uint32_t>(slots_.size()); }
  size_t slot_capacity() const { return slot_capacity_; }
  size_t mapped_bytes() const { return mapped_bytes_; }
  bool slot_in_flight(int slot) const;

 private:
  struct Slot {
    uint64_t frame_id = 0;
    bool in_flight = false;
  };

  int fd_ = -1;
  void *mapping_ = nullptr;
  uint64_t generation_ = 0;
  size_t slot_capacity_ = 0;
  size_t mapped_bytes_ = 0;
  std::vector<Slot> slots_;
};

#endif
