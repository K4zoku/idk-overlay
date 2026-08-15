#include <stdint.h>

#include "hook/x11_internal.h"
#include "test_runner.h"

static Display *received_display;
static Window received_window;
static long received_mask;
static long server_your_mask;
static long server_all_mask;
static int attributes_status;
static int select_calls;
static int attributes_calls;
static int sync_calls;

static int fake_XSelectInput(Display *dpy, Window window, long mask) {
  received_display = dpy;
  received_window = window;
  received_mask = mask;
  select_calls++;
  return 17;
}

static int fake_XGetWindowAttributes(Display *dpy, Window window, void *storage) {
  (void)dpy;
  (void)window;
  attributes_calls++;
  if (!attributes_status)
    return 0;
  XWindowAttributesLayout *attrs = storage;
  memset(attrs, 0, sizeof(*attrs));
  attrs->your_event_mask = server_your_mask;
  attrs->all_event_masks = server_all_mask;
  return 1;
}

static int fake_XSync(Display *dpy, Bool discard) {
  (void)dpy;
  (void)discard;
  sync_calls++;
  return 0;
}

XSelectInput_fn orig_XSelectInput = fake_XSelectInput;
XGetWindowAttributes_fn fn_XGetWindowAttributes = fake_XGetWindowAttributes;
XSync_fn fn_XSync = fake_XSync;

void *real_dlsym(void *handle, const char *symbol) {
  (void)handle;
  (void)symbol;
  return NULL;
}

static void reset_state(void) {
  received_display = NULL;
  received_window = 0;
  received_mask = 0;
  server_your_mask = NoEventMask;
  server_all_mask = NoEventMask;
  attributes_status = 1;
  select_calls = 0;
  attributes_calls = 0;
  sync_calls = 0;
}

TEST(select_input_claims_unowned_button_press) {
  reset_state();
  Display *display = (Display *)(uintptr_t)0x1000;
  long requested = KeyPressMask | ExposureMask;
  server_your_mask = requested;
  server_all_mask = requested;

  ASSERT_EQ(hook_XSelectInput(display, 0x101, requested), 17);

  ASSERT_TRUE(received_display == display);
  ASSERT_EQ(received_window, 0x101);
  ASSERT_EQ(received_mask, requested | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
  ASSERT_EQ(attributes_calls, 1);
  ASSERT_EQ(sync_calls, 2);
}

TEST(select_input_avoids_foreign_button_owner) {
  reset_state();
  Display *display = (Display *)(uintptr_t)0x2000;
  long requested = KeyPressMask | ExposureMask;
  server_your_mask = requested;
  server_all_mask = requested | ButtonPressMask;

  hook_XSelectInput(display, 0x201, requested);

  ASSERT_EQ(received_mask, requested | KeyReleaseMask | ButtonReleaseMask | PointerMotionMask);
  ASSERT_FALSE(received_mask & ButtonPressMask);
}

TEST(select_input_preserves_caller_button_press) {
  reset_state();
  Display *display = (Display *)(uintptr_t)0x3000;
  long requested = ButtonPressMask | StructureNotifyMask;
  server_all_mask = ButtonPressMask;

  hook_XSelectInput(display, 0x301, requested);

  ASSERT_EQ(received_mask, requested | KeyReleaseMask | ButtonReleaseMask | PointerMotionMask);
  ASSERT_TRUE(received_mask & ButtonPressMask);
}

TEST(retroactive_masks_preserve_current_selection) {
  reset_state();
  Display *display = (Display *)(uintptr_t)0x4000;
  server_your_mask = KeyPressMask | ExposureMask;
  server_all_mask = server_your_mask;

  x11_ensure_event_masks(display, 0x401);

  ASSERT_TRUE(received_display == display);
  ASSERT_EQ(received_window, 0x401);
  ASSERT_EQ(received_mask, server_your_mask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
  ASSERT_EQ(select_calls, 1);
  ASSERT_EQ(sync_calls, 2);
}

TEST(retroactive_masks_avoid_foreign_button_owner) {
  reset_state();
  Display *display = (Display *)(uintptr_t)0x5000;
  server_your_mask = KeyPressMask;
  server_all_mask = KeyPressMask | ButtonPressMask;

  x11_ensure_event_masks(display, 0x501);

  ASSERT_EQ(received_mask, KeyPressMask | KeyReleaseMask | ButtonReleaseMask | PointerMotionMask);
  ASSERT_FALSE(received_mask & ButtonPressMask);
}

TEST(retroactive_masks_require_existing_selection) {
  reset_state();
  Display *display = (Display *)(uintptr_t)0x6000;
  attributes_status = 0;

  x11_ensure_event_masks(display, 0x601);

  ASSERT_EQ(select_calls, 0);
}

TEST(retroactive_masks_cache_display_window_pair) {
  reset_state();
  Display *display = (Display *)(uintptr_t)0x7000;
  server_your_mask = KeyPressMask;
  server_all_mask = KeyPressMask;

  x11_ensure_event_masks(display, 0x701);
  x11_ensure_event_masks(display, 0x701);

  ASSERT_EQ(select_calls, 1);
}

int main(void) {
  RUN(select_input_claims_unowned_button_press);
  RUN(select_input_avoids_foreign_button_owner);
  RUN(select_input_preserves_caller_button_press);
  RUN(retroactive_masks_preserve_current_selection);
  RUN(retroactive_masks_avoid_foreign_button_owner);
  RUN(retroactive_masks_require_existing_selection);
  RUN(retroactive_masks_cache_display_window_pair);
  return 0;
}
