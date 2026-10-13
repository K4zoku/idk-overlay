/* Persistent CPU SHM ring wire records. */
#ifndef IDK_SHM_RING_H
#define IDK_SHM_RING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IDK_SHM_RING_MAGIC 0x31475253u
#define IDK_SHM_RING_VERSION 1u
#define IDK_SHM_RING_MAX_SLOTS 3u

enum idk_shm_ring_record_type {
  IDK_SHM_RING_CREATE = 1,
  IDK_SHM_RING_READY = 2,
  IDK_SHM_RING_FRAME_SLOT = 3,
  IDK_SHM_RING_SLOT_RELEASE = 4,
  IDK_SHM_RING_RETIRE = 5,
};

enum idk_shm_ring_frame_flags {
  IDK_SHM_RING_FRAME_FULL = 1u << 0,
  IDK_SHM_RING_FRAME_ROI = 1u << 1,
};

#pragma pack(push, 1)
typedef struct idk_shm_ring_record {
  uint16_t type;
  uint16_t flags;
  uint32_t payload_bytes;
  uint64_t session_generation;
  uint64_t message_id;
} idk_shm_ring_record_t;

typedef struct idk_shm_ring_create {
  uint32_t magic;
  uint16_t version;
  uint16_t slot_count;
  uint32_t slot_capacity;
  uint64_t ring_bytes;
  uint64_t session_generation;
  uint32_t width;
  uint32_t height;
  uint32_t stride;
  uint32_t pixel_format;
  uint32_t reserved;
} idk_shm_ring_create_t;

typedef struct idk_shm_ring_ready {
  uint64_t session_generation;
  uint16_t slot_count;
  uint16_t reserved;
  uint32_t reserved2;
} idk_shm_ring_ready_t;

typedef struct idk_shm_ring_frame_slot {
  uint64_t session_generation;
  uint64_t frame_id;
  uint64_t base_frame_id;
  uint32_t slot_index;
  uint32_t offset;
  uint32_t payload_bytes;
  uint32_t width;
  uint32_t height;
  uint32_t stride;
  uint32_t pixel_format;
  uint32_t x;
  uint32_t y;
  uint32_t roi_width;
  uint32_t roi_height;
  uint16_t flags;
  uint16_t reserved;
} idk_shm_ring_frame_slot_t;

typedef struct idk_shm_ring_slot_release {
  uint64_t session_generation;
  uint64_t frame_id;
  uint32_t slot_index;
  uint32_t status;
} idk_shm_ring_slot_release_t;

typedef struct idk_shm_ring_retire {
  uint64_t session_generation;
  uint32_t reason;
  uint32_t reserved;
} idk_shm_ring_retire_t;
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(idk_shm_ring_record_t) == 24, "idk_shm_ring_record_t ABI");
static_assert(sizeof(idk_shm_ring_create_t) == 48, "idk_shm_ring_create_t ABI");
static_assert(sizeof(idk_shm_ring_ready_t) == 16, "idk_shm_ring_ready_t ABI");
static_assert(sizeof(idk_shm_ring_frame_slot_t) == 72, "idk_shm_ring_frame_slot_t ABI");
static_assert(sizeof(idk_shm_ring_slot_release_t) == 24, "idk_shm_ring_slot_release_t ABI");
static_assert(sizeof(idk_shm_ring_retire_t) == 16, "idk_shm_ring_retire_t ABI");
#else
_Static_assert(sizeof(idk_shm_ring_record_t) == 24, "idk_shm_ring_record_t ABI");
_Static_assert(sizeof(idk_shm_ring_create_t) == 48, "idk_shm_ring_create_t ABI");
_Static_assert(sizeof(idk_shm_ring_ready_t) == 16, "idk_shm_ring_ready_t ABI");
_Static_assert(sizeof(idk_shm_ring_frame_slot_t) == 72, "idk_shm_ring_frame_slot_t ABI");
_Static_assert(sizeof(idk_shm_ring_slot_release_t) == 24, "idk_shm_ring_slot_release_t ABI");
_Static_assert(sizeof(idk_shm_ring_retire_t) == 16, "idk_shm_ring_retire_t ABI");
#endif

#ifdef __cplusplus
}
#endif

#endif
