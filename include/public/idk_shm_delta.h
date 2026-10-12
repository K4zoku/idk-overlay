/* Optional SHM rectangle transport. Frame header retains its 28-byte ABI.
 * A frame with IDK_FRAME_FLAG_SHM_RECT stores this 20-byte header followed
 * by tightly packed RGBA bytes (same byte order as a full SHM frame).
 * width/height in idk_frame_header_t still denote the FULL canvas.
 */
#ifndef IDK_SHM_DELTA_H
#define IDK_SHM_DELTA_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>
#include "public/idk_frames.h"

#define IDK_SHM_DELTA_MAGIC 0x31524449u /* "IDR1" little-endian */
#define IDK_FRAME_FLAG_SHM_RECT 0x04

#pragma pack(push, 1)
typedef struct idk_shm_delta {
  uint32_t magic;
  uint32_t x;
  uint32_t y;
  uint32_t width;
  uint32_t height;
} idk_shm_delta_t;
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(idk_shm_delta_t) == 20, "SHM delta ABI");
#else
_Static_assert(sizeof(idk_shm_delta_t) == 20, "SHM delta ABI");
#endif

static inline bool idk_shm_delta_validate(const idk_shm_delta_t *r, uint32_t w, uint32_t h, size_t mapped_size) {
  if (!r || r->magic != IDK_SHM_DELTA_MAGIC || !w || !h || !r->width || !r->height || r->x >= w || r->y >= h ||
      r->width > w - r->x || r->height > h - r->y)
    return false;
  uint64_t len = (uint64_t)r->width * r->height * 4u + sizeof(*r);
  return len <= SIZE_MAX && len == mapped_size;
}

/* Fill the tightly packed rectangle body; caller allocates header + body.
 * Assumes source contains a full canvas in row-major RGBA/BGRA byte order. */
static inline bool idk_shm_delta_pack(void *dst, size_t capacity, const void *source,
                                      uint32_t w, uint32_t h, const idk_shm_delta_t *roi) {
  if (!dst || !source || !idk_shm_delta_validate(roi, w, h, capacity))
    return false;
  memcpy(dst, roi, sizeof(*roi));
  uint8_t *out = (uint8_t *)dst + sizeof(*roi);
  const uint8_t *in = (const uint8_t *)source;
  for (uint32_t row = 0; row < roi->height; ++row)
    memcpy(out + (size_t)row * roi->width * 4,
           in + ((size_t)(roi->y + row) * w + roi->x) * 4, (size_t)roi->width * 4);
  return true;
}

#endif
