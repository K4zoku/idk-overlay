#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

#include "../src/cli/webview_cef/shm_format.h"
#include "public/idk_shm_delta.h"

int main() {
  // Top-left BGRA: top cyan, top blue; bottom orange, bottom red.
  const std::array<uint8_t, 16> bgra = {
      255, 255, 0, 255, 255, 0, 0, 255,
      0, 128, 255, 255, 0, 0, 255, 255,
  };
  std::vector<uint8_t> gl;
  assert(idk_cef_shm::ToGlRgba(bgra.data(), 8, 2, 2, idk_cef_shm::Channels::BGRA, gl));
  const std::array<uint8_t, 16> expected = {
      255, 128, 0, 255, 255, 0, 0, 255,
      0, 255, 255, 255, 0, 0, 255, 255,
  };
  assert(std::equal(gl.begin(), gl.end(), expected.begin()));
  assert(idk_cef_shm::ToGlY(0, 1, 2) == 1);
  assert(idk_cef_shm::ToGlY(1, 1, 2) == 0);

  // ROI for the top row must update the LAST GL texel row.
  idk_shm_delta_t roi{IDK_SHM_DELTA_MAGIC, 0, 1, 2, 1};
  std::array<uint8_t, sizeof(roi) + 8> packet{};
  assert(idk_shm_delta_pack(packet.data(), packet.size(), gl.data(), 2, 2, &roi));
  const uint8_t *roi_pixels = packet.data() + sizeof(roi);
  assert(std::equal(roi_pixels, roi_pixels + 8, expected.begin() + 8));

  // RGBA input is also top-down in CEF's CPU-visible pixmap representation.
  std::vector<uint8_t> rgba;
  const std::array<uint8_t, 16> top_down_rgba = {
      0, 255, 255, 255, 0, 0, 255, 255,
      255, 128, 0, 255, 255, 0, 0, 255,
  };
  assert(idk_cef_shm::ToGlRgba(top_down_rgba.data(), 8, 2, 2,
                                idk_cef_shm::Channels::RGBA, rgba));
  assert(rgba == gl);
  assert(!idk_cef_shm::ToGlRgba(top_down_rgba.data(), 7, 2, 2,
                                idk_cef_shm::Channels::RGBA, rgba));
  return 0;
}
