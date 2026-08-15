#include "test_runner.h"

#include <pthread.h>
#include <stdint.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core/transport.h"

static idk_cursor_update_t custom_cursor(void) {
  idk_cursor_update_t cursor = {
      .magic = IDK_CURSOR_MAGIC,
      .version = IDK_CURSOR_VERSION,
      .visible = 1,
      .shape = IDK_CURSOR_CUSTOM,
      .width = 4,
      .height = 2,
      .hotspot_x = 1,
      .hotspot_y = 1,
      .scale = IDK_CURSOR_SCALE_BASE,
      .data_size = 4 * 2 * 4,
  };
  return cursor;
}

static void assert_cursor_equal(const idk_cursor_update_t *got, const idk_cursor_update_t *expected) {
  ASSERT_EQ(got->magic, expected->magic);
  ASSERT_EQ(got->version, expected->version);
  ASSERT_EQ(got->visible, expected->visible);
  ASSERT_EQ(got->shape, expected->shape);
  ASSERT_EQ(got->width, expected->width);
  ASSERT_EQ(got->height, expected->height);
  ASSERT_EQ(got->hotspot_x, expected->hotspot_x);
  ASSERT_EQ(got->hotspot_y, expected->hotspot_y);
  ASSERT_EQ(got->scale, expected->scale);
  ASSERT_EQ(got->data_size, expected->data_size);
}

TEST(socket_cursor_roundtrip) {
  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  idk_transport_t tx = {
      .role = IDK_TP_PRODUCER,
      .backend = IDK_TP_SOCKET,
      .ready = true,
      ._server_fd = -1,
      ._client_fd = fds[0],
  };
  idk_transport_t rx = {
      .role = IDK_TP_CONSUMER,
      .backend = IDK_TP_SOCKET,
      .ready = true,
      ._server_fd = -1,
      ._client_fd = fds[1],
  };
  uint8_t pixels[32];
  for (size_t i = 0; i < sizeof(pixels); i++)
    pixels[i] = (uint8_t)(i * 7);
  idk_cursor_update_t sent = custom_cursor();
  ASSERT_EQ(idk_tp_send_cursor(&tx, &sent, pixels), 0);
  idk_cursor_update_t received;
  uint8_t received_pixels[32] = {0};
  ASSERT_EQ(idk_tp_recv_cursor(&rx, &received, received_pixels, sizeof(received_pixels)), 1);
  assert_cursor_equal(&received, &sent);
  ASSERT_EQ(memcmp(received_pixels, pixels, sizeof(pixels)), 0);
  idk_tp_destroy(&tx);
  idk_tp_destroy(&rx);
}

struct producer_init {
  idk_transport_t *tp;
  const char *name;
  int result;
};

static void *init_producer(void *data) {
  struct producer_init *init = data;
  init->result = idk_tp_init(init->tp, IDK_TP_PRODUCER, init->name);
  return NULL;
}

TEST(shm_cursor_roundtrip) {
  ASSERT_EQ(setenv("IDK_TP_BACKEND", "shm", 1), 0);
  char name[64];
  snprintf(name, sizeof(name), "/tmp/idk_cursor_test_%d", (int)getpid());
  idk_transport_t consumer = {0};
  idk_transport_t producer = {0};
  ASSERT_EQ(idk_tp_init(&consumer, IDK_TP_CONSUMER, name), 0);
  struct producer_init init = {.tp = &producer, .name = name, .result = -1};
  pthread_t thread;
  ASSERT_EQ(pthread_create(&thread, NULL, init_producer, &init), 0);
  for (int i = 0; i < 1000 && !consumer.ready; i++) {
    ASSERT_TRUE(idk_tp_accept(&consumer) >= 0);
    usleep(1000);
  }
  ASSERT_TRUE(consumer.ready);
  ASSERT_EQ(pthread_join(thread, NULL), 0);
  ASSERT_EQ(init.result, 0);

  uint8_t pixels[32];
  for (size_t i = 0; i < sizeof(pixels); i++)
    pixels[i] = (uint8_t)(255 - i * 3);
  idk_cursor_update_t sent = custom_cursor();
  ASSERT_EQ(idk_tp_send_cursor(&producer, &sent, pixels), 0);
  idk_cursor_update_t received;
  uint8_t received_pixels[32] = {0};
  ASSERT_EQ(idk_tp_recv_cursor(&consumer, &received, received_pixels, sizeof(received_pixels)), 1);
  assert_cursor_equal(&received, &sent);
  ASSERT_EQ(memcmp(received_pixels, pixels, sizeof(pixels)), 0);

  idk_tp_destroy(&producer);
  idk_tp_destroy(&consumer);
  unsetenv("IDK_TP_BACKEND");
}

int main(void) {
  RUN(socket_cursor_roundtrip);
  RUN(shm_cursor_roundtrip);
  return 0;
}
