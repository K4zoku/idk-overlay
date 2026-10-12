#include <assert.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core/compositor.h"

int main(void) {
  int fd[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
  memset(&g_comp, 0, sizeof(g_comp));
  g_comp.tp.backend = IDK_TP_SOCKET;
  g_comp.tp._client_fd = fd[0];
  g_comp.tp._server_fd = -1;
  g_comp.tp.ready = true;
  g_comp.inited = true;

  idk_transport_t producer = {0};
  producer.backend = IDK_TP_SOCKET;
  producer._client_fd = fd[1];
  producer._server_fd = -1;
  producer.ready = true;

  int framefd = open("/dev/null", O_RDONLY);
  assert(framefd >= 0);
  idk_frame_header_t frame = {.width = 32, .height = 32, .nfd = 1};
  assert(idk_tp_send(&producer, &frame, &framefd, 1) == 0);
  close(framefd);

  // The game hides the overlay instead of consuming/rendering the frame.
  assert(idk_compositor_recv_frame(false) == 0);
  idk_ack_msg_t ack = {0};
  assert(idk_tp_wait_ack(&producer, &ack, 0) == 0);
  assert(ack.ack == IDK_ACK_DISCARDED);
  assert(idk_compositor_recv_frame(false) == 0);
  assert(idk_tp_wait_ack(&producer, &ack, 0) != 0);

  g_comp.inited = false;
  idk_tp_destroy(&producer);
  idk_tp_destroy(&g_comp.tp);
  return 0;
}
