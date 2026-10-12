#include <cassert>
#include <cstdint>
#include <vector>

#include "../src/cli/webview_cef/shm_diff.h"

int main() {
  constexpr uint32_t w = 96, h = 64;
  std::vector<uint8_t> baseline(w * h * 4, 0);
  std::vector<uint8_t> current = baseline;
  idk_shm_delta_t roi{};
  using idk_cef_shm::DeltaResult;
  using idk_cef_shm::FindDelta;
  assert(FindDelta(current.data(), baseline.data(), w, h, roi) == DeltaResult::Unchanged);
  for (uint32_t y = 7; y < 17; ++y)
    for (uint32_t x = 15; x < 30; ++x)
      current[((size_t)y * w + x) * 4 + 2] = 255;
  assert(FindDelta(current.data(), baseline.data(), w, h, roi) == DeltaResult::Partial);
  assert(roi.x == 15 && roi.y == 7 && roi.width == 15 && roi.height == 10);
  std::vector<uint8_t> packet(sizeof(roi) + (size_t)roi.width * roi.height * 4);
  assert(idk_shm_delta_pack(packet.data(), packet.size(), current.data(), w, h, &roi));
  // Add distant damage; single bounding rectangle now exceeds half the view.
  current[((size_t)(h - 1) * w + w - 1) * 4] = 21;
  assert(FindDelta(current.data(), baseline.data(), w, h, roi) == DeltaResult::Full);
  baseline = current;
  assert(FindDelta(current.data(), baseline.data(), w, h, roi) == DeltaResult::Unchanged);
  return 0;
}
