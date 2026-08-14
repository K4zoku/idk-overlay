/* End-to-end: spawn idk-webview-cef against a fake compositor socket and
 * verify the dmabuf frame flow (REQUEST → frame → ACK) plus SHUTDOWN.
 *
 * Usage: test_webview_cef <path-to-idk-webview-cef>
 * Requires a display (CEF GPU process) and the CEF dist in the binary's
 * rpath — built only when -Dcef_dist is set.
 */
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core/transport.h"
#include "public/idk_frames.h"
#include "test_runner.h"

#define SOCK_PATH "/tmp/idk-cef-itest.sock"
#define ACCEPT_TIMEOUT_MS 30000
#define FRAME_TIMEOUT_MS 15000

/* Animated page: each external begin frame advances an rAF tick → new
 * damage → new frame. A static page would only produce one frame. */
#define ANIM_URL                                                                                                       \
  "data:text/html,%3Cbody%20style%3D%22margin%3A0%22%3E%3Cscript%3E"                                                   \
  "let%20c%3D0%3Bfunction%20f()%7Bdocument.body.style.background%3D"                                                   \
  "(c%2B%2B%252)%3F%27%230f0%27%3A%27%2300f%27%3B"                                                                     \
  "requestAnimationFrame(f)%7DrequestAnimationFrame(f)%3C%2Fscript%3E"                                                 \
  "%3C%2Fbody%3E"

static const char *argv_bin;

static int wait_readable(int fd, int timeout_ms) {
  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  return poll(&pfd, 1, timeout_ms) > 0;
}

static int wait_accept(idk_transport_t *tp, int timeout_ms) {
  int waited = 0;
  while (waited < timeout_ms) {
    if (wait_readable(tp->_server_fd, 250)) {
      int rc = idk_tp_accept(tp);
      if (rc > 0)
        return 0;
    }
    waited += 250;
  }
  return -1;
}

/* Recv one frame; assert it is a valid multi-plane dmabuf. */
static void verify_frame(idk_transport_t *tp) {
  idk_frame_header_t hdr;
  int fds[4] = {-1, -1, -1, -1};
  int nfd = 0;
  ASSERT_EQ(idk_tp_recv(tp, &hdr, fds, &nfd), 1);

  ASSERT_TRUE(hdr.width > 0 && hdr.height > 0);
  ASSERT_TRUE(hdr.flags & IDK_FRAME_FLAG_DMABUF);
  ASSERT_TRUE(hdr.nfd >= 1 && hdr.nfd <= 4);
  ASSERT_EQ(nfd, hdr.nfd);
  ASSERT_TRUE(hdr.fourcc != 0);
  ASSERT_TRUE(hdr.stride > 0);

  char link[64], target[128];
  snprintf(link, sizeof(link), "/proc/self/fd/%d", fds[0]);
  ssize_t n = readlink(link, target, sizeof(target) - 1);
  ASSERT_TRUE(n > 0);
  target[n] = '\0';
  ASSERT_TRUE(strstr(target, "dmabuf") != NULL);

  for (int i = 0; i < nfd; i++)
    close(fds[i]);
}

TEST(cef_webview_frame_flow) {
  unlink(SOCK_PATH);

  idk_transport_t comp;
  memset(&comp, 0, sizeof(comp));
  ASSERT_EQ(idk_tp_init(&comp, IDK_TP_CONSUMER, SOCK_PATH), 0);

  pid_t pid = fork();
  ASSERT_TRUE(pid >= 0);
  if (pid == 0) {
    execl(argv_bin, argv_bin, "--socket", SOCK_PATH, "--url", ANIM_URL, (char *)NULL);
    _exit(127);
  }

  /* CEF init takes ~1-2s; the webview connects when its producer socket
   * comes up. */
  ASSERT_EQ(wait_accept(&comp, ACCEPT_TIMEOUT_MS), 0);

  idk_request_msg_t req = {.type = IDK_REQUEST_NEXT_FRAME};
  for (int i = 0; i < 3; i++) {
    ASSERT_EQ(idk_tp_send_request(&comp, &req), 0);
    ASSERT_TRUE(wait_readable(comp._client_fd, FRAME_TIMEOUT_MS));
    verify_frame(&comp);
    idk_ack_msg_t ack = {.ack = 0, .w = 0, .h = 0};
    idk_tp_send_ack(&comp, &ack);
  }

  idk_request_msg_t sd = {.type = IDK_REQUEST_SHUTDOWN};
  ASSERT_EQ(idk_tp_send_request(&comp, &sd), 0);

  int status = 0;
  pid_t r = waitpid(pid, &status, 0);
  ASSERT_TRUE(r == pid);
  /* Webview teardown may end in either exit or signal; it must die. */
  ASSERT_TRUE(WIFEXITED(status) || WIFSIGNALED(status));

  idk_tp_destroy(&comp);
  unlink(SOCK_PATH);
}

/* Reject every dmabuf frame: after 5 rejections the webview must switch
 * to SHM frames (memfd, no DMABUF flag, zero stride/fourcc). */
TEST(cef_webview_shm_fallback) {
  unlink(SOCK_PATH);

  idk_transport_t comp;
  memset(&comp, 0, sizeof(comp));
  ASSERT_EQ(idk_tp_init(&comp, IDK_TP_CONSUMER, SOCK_PATH), 0);

  pid_t pid = fork();
  ASSERT_TRUE(pid >= 0);
  if (pid == 0) {
    execl(argv_bin, argv_bin, "--socket", SOCK_PATH, "--url", ANIM_URL, (char *)NULL);
    _exit(127);
  }

  ASSERT_EQ(wait_accept(&comp, ACCEPT_TIMEOUT_MS), 0);

  idk_request_msg_t req = {.type = IDK_REQUEST_NEXT_FRAME};
  int dmabuf_frames = 0, shm_frames = 0;
  for (int i = 0; i < 8; i++) {
    ASSERT_EQ(idk_tp_send_request(&comp, &req), 0);
    ASSERT_TRUE(wait_readable(comp._client_fd, FRAME_TIMEOUT_MS));

    idk_frame_header_t hdr;
    int fds[4] = {-1, -1, -1, -1};
    int nfd = 0;
    ASSERT_EQ(idk_tp_recv(&comp, &hdr, fds, &nfd), 1);

    if (hdr.flags & IDK_FRAME_FLAG_DMABUF) {
      dmabuf_frames++;
      ASSERT_TRUE(hdr.fourcc != 0);
    } else {
      shm_frames++;
      ASSERT_EQ(hdr.stride, 0u);
      ASSERT_EQ(hdr.fourcc, 0u);
      /* Content-freshness check: the rAF page alternates green/blue;
       * a stale render would keep sending the same pixels. */
      size_t sz = (size_t)hdr.width * hdr.height * 4;
      void *map = mmap(NULL, sz, PROT_READ, MAP_SHARED, fds[0], 0);
      if (map != MAP_FAILED) {
        const uint8_t *px =
            (const uint8_t *)map + (size_t)(hdr.height / 2) * hdr.width * 4 + (size_t)(hdr.width / 2) * 4;
        printf("[probe] shm pixel #%02x%02x%02x\n", px[0], px[1], px[2]);
        munmap(map, sz);
      }
    }
    for (int j = 0; j < nfd; j++)
      close(fds[j]);

    idk_ack_msg_t ack = {.ack = 1, .w = 0, .h = 0}; /* reject dmabuf */
    idk_tp_send_ack(&comp, &ack);
  }
  ASSERT_TRUE(dmabuf_frames >= 5);
  ASSERT_TRUE(shm_frames >= 1);

  idk_request_msg_t sd = {.type = IDK_REQUEST_SHUTDOWN};
  ASSERT_EQ(idk_tp_send_request(&comp, &sd), 0);

  int status = 0;
  waitpid(pid, &status, 0);

  idk_tp_destroy(&comp);
  unlink(SOCK_PATH);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <idk-webview-cef binary>\n", argv[0]);
    return 1;
  }
  argv_bin = argv[1];
  RUN(cef_webview_frame_flow);
  RUN(cef_webview_shm_fallback);
  return 0;
}
