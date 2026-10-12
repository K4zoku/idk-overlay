#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "public/idk_shm_delta.h"
int main(void) {
  idk_shm_delta_t r = {IDK_SHM_DELTA_MAGIC, 12, 8, 10, 20};
  assert(idk_shm_delta_validate(&r, 320, 240, sizeof(r) + 10 * 20 * 4));
  assert(!idk_shm_delta_validate(&r, 320, 240, sizeof(r) + 10 * 20 * 4 - 1));
  assert(!idk_shm_delta_validate(&r, 320, 240, sizeof(r) + 10 * 20 * 4 + 1));
  r.x = 315;
  assert(!idk_shm_delta_validate(&r, 320, 240, sizeof(r) + 10 * 20 * 4));
  r.x = 12; r.y = UINT32_MAX;
  assert(!idk_shm_delta_validate(&r, 320, 240, sizeof(r) + 10 * 20 * 4));
  r.y = 8; r.width = UINT32_MAX;
  assert(!idk_shm_delta_validate(&r, 320, 240, sizeof(r) + 10 * 20 * 4));
  r.width = 10; r.magic = 0;
  assert(!idk_shm_delta_validate(&r, 320, 240, sizeof(r) + 10 * 20 * 4));

  /* Two-by-two region from a known 4x4 canvas; enforce exact pixel order. */
  uint8_t canvas[4 * 4 * 4];
  for (int i = 0; i < 16; ++i)
    for (int c = 0; c < 4; ++c)
      canvas[i * 4 + c] = i;
  idk_shm_delta_t region = {IDK_SHM_DELTA_MAGIC, 1, 1, 2, 2};
  uint8_t packet[sizeof(region) + 2 * 2 * 4];
  assert(idk_shm_delta_pack(packet, sizeof(packet), canvas, 4, 4, &region));
  idk_shm_delta_t unpacked;
  memcpy(&unpacked, packet, sizeof(unpacked));
  assert(idk_shm_delta_validate(&unpacked, 4, 4, sizeof(packet)));
  const uint8_t *body = packet + sizeof(region);
  assert(body[0] == 5 && body[4] == 6 && body[8] == 9 && body[12] == 10);
  assert(!idk_shm_delta_pack(packet, sizeof(packet) - 1, canvas, 4, 4, &region));
  return 0;
}
