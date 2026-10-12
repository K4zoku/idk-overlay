#pragma once

#include <cstdint>
#include <cstring>

#include "public/idk_shm_delta.h"

namespace idk_cef_shm {

enum class DeltaResult { Unchanged, Full, Partial };

/* Compare the newest GL-row-order RGBA snapshot against the LAST ACKED
 * canvas. Unlike CEF dirtyRects, this also covers all deferred paints and
 * skipped captures; one bounding ROI is sent only if cheaper than full. */
inline DeltaResult FindDelta(const uint8_t *current, const uint8_t *baseline,
                             uint32_t w, uint32_t h, idk_shm_delta_t &roi) {
  if (!current || !baseline || !w || !h || (uint64_t)w * h * 4 > SIZE_MAX)
    return DeltaResult::Full;
  uint32_t x0 = w, y0 = h, x1 = 0, y1 = 0;
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t *a = current + (size_t)y * w * 4;
    const uint8_t *b = baseline + (size_t)y * w * 4;
    if (memcmp(a, b, (size_t)w * 4) == 0)
      continue;
    for (uint32_t x = 0; x < w; ++x) {
      if (memcmp(a + (size_t)x * 4, b + (size_t)x * 4, 4) != 0) {
        if (x < x0) x0 = x;
        if (y < y0) y0 = y;
        if (x + 1 > x1) x1 = x + 1;
        if (y + 1 > y1) y1 = y + 1;
      }
    }
  }
  if (x0 == w)
    return DeltaResult::Unchanged;
  if ((uint64_t)(x1 - x0) * (y1 - y0) * 2 >= (uint64_t)w * h)
    return DeltaResult::Full;
  roi = {IDK_SHM_DELTA_MAGIC, x0, y0, x1 - x0, y1 - y0};
  return DeltaResult::Partial;
}

} // namespace idk_cef_shm
