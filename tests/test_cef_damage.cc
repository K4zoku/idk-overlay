#include <cassert>
#include <climits>

#include "../src/cli/webview_cef/damage.h"

int main() {
  IdkBlitRect r{};
  assert(idk_dirty_blit_rect(10, 20, 30, 40, 200, 100, r));
  assert(r.sx0 == 10 && r.sy0 == 20 && r.sx1 == 40 && r.sy1 == 60);
  assert(r.dx0 == 10 && r.dy0 == 80 && r.dx1 == 40 && r.dy1 == 40);
  assert(idk_dirty_blit_rect(-5, 90, 20, 20, 200, 100, r));
  assert(r.sx0 == 0 && r.sy0 == 90 && r.sx1 == 15 && r.sy1 == 100);
  assert(r.dy0 == 10 && r.dy1 == 0);
  assert(!idk_dirty_blit_rect(210, 0, 10, 10, 200, 100, r));
  assert(!idk_dirty_blit_rect(0, 0, 0, 20, 200, 100, r));
  assert(!idk_dirty_blit_rect(INT_MAX, INT_MAX, INT_MAX, INT_MAX, 200, 100, r));
  assert(idk_dirty_blit_rect(0, 0, 200, 100, 200, 100, r));
  assert(r.dx0 == 0 && r.dy0 == 100 && r.dx1 == 200 && r.dy1 == 0);
  return 0;
}
