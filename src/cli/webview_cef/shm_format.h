#pragma once

#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

/* The existing idk SHM wire format is GL-upload-ready RGBA, with row zero
 * corresponding to the bottom GL texel row (as produced by Qt's glReadPixels).
 * CEF OSR pixels use a TOP-left origin and software OnPaint is BGRA.
 * Normalize at the CEF producer, leaving Qt and both game renderers alone. */
namespace idk_cef_shm {

enum class Channels { RGBA, BGRA };

inline bool ToGlRgba(const uint8_t *source, uint32_t stride, int width, int height,
                     Channels channels, std::vector<uint8_t> &result) {
  if (!source || width <= 0 || height <= 0 ||
      (uint64_t)width * height * 4 > 256ull * 1024 * 1024 ||
      (uint64_t)width * 4 > stride)
    return false;
  const size_t bytes_per_row = (size_t)width * 4;
  result.resize(bytes_per_row * (size_t)height);
  for (int y = 0; y < height; ++y) {
    const uint8_t *src = source + (size_t)y * stride;
    uint8_t *dst = result.data() + (size_t)(height - 1 - y) * bytes_per_row;
    if (channels == Channels::RGBA) {
      memcpy(dst, src, bytes_per_row);
    } else {
      for (int x = 0; x < width; ++x) {
        dst[x * 4 + 0] = src[x * 4 + 2];
        dst[x * 4 + 1] = src[x * 4 + 1];
        dst[x * 4 + 2] = src[x * 4 + 0];
        dst[x * 4 + 3] = src[x * 4 + 3];
      }
    }
  }
  return true;
}

/* CEF dirtyRects use a top-left origin; the SHM ROI packet and glTexSubImage2D
 * use bottom-left row coordinates. Call after clipping to the view bounds. */
inline int ToGlY(int top, int rect_height, int canvas_height) {
  return canvas_height - top - rect_height;
}

} // namespace idk_cef_shm
