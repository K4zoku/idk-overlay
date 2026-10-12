#pragma once
#include <algorithm>
#include <cstdint>

struct IdkBlitRect {
  int sx0, sy0, sx1, sy1;
  int dx0, dy0, dx1, dy1;
};

// CEF reports dirty rectangles in top-left coordinates. The existing GPU
// staging blit vertically flips the input; preserve that transform for ROIs.
inline bool idk_dirty_blit_rect(int x, int y, int width, int height, int frame_w, int frame_h, IdkBlitRect &out) {
  if (frame_w <= 0 || frame_h <= 0 || width <= 0 || height <= 0)
    return false;
  int64_t x0 = std::max<int64_t>(0, x);
  int64_t y0 = std::max<int64_t>(0, y);
  int64_t x1 = std::min<int64_t>(frame_w, int64_t(x) + width);
  int64_t y1 = std::min<int64_t>(frame_h, int64_t(y) + height);
  if (x1 <= x0 || y1 <= y0)
    return false;
  out = {(int)x0, (int)y0, (int)x1, (int)y1, (int)x0, frame_h - (int)y0,
         (int)x1, frame_h - (int)y1};
  return true;
}
