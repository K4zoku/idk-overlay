#include <assert.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core/transport.h"

static void peers(idk_transport_t *game, idk_transport_t *web) {
  int fd[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
  memset(game, 0, sizeof(*game));
  memset(web, 0, sizeof(*web));
  game->backend = IDK_TP_SOCKET;
  web->backend = IDK_TP_SOCKET;
  game->ready = web->ready = true;
  game->_client_fd = fd[0];
  web->_client_fd = fd[1];
  game->_server_fd = -1;
  web->_server_fd = -1;
}

int main(void) {
  idk_transport_t game, web;
  peers(&game, &web);
  idk_request_msg_t req = {.type = IDK_REQUEST_NEXT_FRAME};
  idk_ack_msg_t ack = {.ack = 1, .w = 3840, .h = 2160};
  for (int i = 0; i < 5; i++) assert(idk_tp_send_request(&game, &req) == 0);
  idk_tp_send_ack(&game, &ack);
  idk_ack_msg_t got;
  assert(idk_tp_wait_ack(&web, &got, 0) == 0);
  assert(got.ack == 1 && got.w == 3840 && got.h == 2160);
  idk_request_msg_t incoming;
  assert(idk_tp_recv_request(&web, &incoming, 0) == 0);
  assert(incoming.type == IDK_REQUEST_NEXT_FRAME);
  assert(idk_tp_recv_request(&web, &incoming, 0) != 0);
  idk_tp_destroy(&game);
  idk_tp_destroy(&web);

  peers(&game, &web);
  req.type = IDK_REQUEST_NEXT_FRAME;
  assert(idk_tp_send_request(&game, &req) == 0);
  req.type = IDK_REQUEST_SHUTDOWN;
  assert(idk_tp_send_request(&game, &req) == 0);
  ack.ack = 0;
  idk_tp_send_ack(&game, &ack);
  assert(idk_tp_wait_ack(&web, &got, 0) == 0);
  assert(idk_tp_recv_request(&web, &incoming, 0) == 0);
  assert(incoming.type == IDK_REQUEST_SHUTDOWN);
  idk_tp_destroy(&game);
  idk_tp_destroy(&web);

  /* Pending ACK must never be consumed as a stale record when polling control. */
  peers(&game, &web);
  ack.ack = 0;
  idk_tp_send_ack(&game, &ack);
  req.type = IDK_REQUEST_NEXT_FRAME;
  assert(idk_tp_send_request(&game, &req) == 0);
  assert(idk_tp_recv_request_preserving_ack(&web, &incoming, 0) != 0);
  assert(idk_tp_wait_ack(&web, &got, 0) == 0);
  assert(got.ack == IDK_ACK_ACCEPTED);
  assert(idk_tp_recv_request(&web, &incoming, 0) == 0);
  assert(incoming.type == IDK_REQUEST_NEXT_FRAME);
  idk_tp_destroy(&game);
  idk_tp_destroy(&web);

  /* Paused game: shutdown before ACK remains actionable without a timeout. */
  peers(&game, &web);
  req.type = IDK_REQUEST_NEXT_FRAME;
  assert(idk_tp_send_request(&game, &req) == 0);
  req.type = IDK_REQUEST_SHUTDOWN;
  assert(idk_tp_send_request(&game, &req) == 0);
  assert(idk_tp_wait_ack(&web, &got, 0) != 0);
  assert(idk_tp_recv_request_preserving_ack(&web, &incoming, 0) == 0);
  assert(incoming.type == IDK_REQUEST_SHUTDOWN);
  idk_tp_destroy(&game);
  idk_tp_destroy(&web);

  /* Discard ACK has its own status; it is not a DMA-BUF rejection. */
  peers(&game, &web);
  ack = (idk_ack_msg_t){.ack = IDK_ACK_DISCARDED};
  idk_tp_send_ack(&game, &ack);
  assert(idk_tp_wait_ack(&web, &got, 0) == 0);
  assert(got.ack == IDK_ACK_DISCARDED);
  idk_tp_destroy(&game);
  idk_tp_destroy(&web);
  return 0;
}
