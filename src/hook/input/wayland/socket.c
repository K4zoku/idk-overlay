#include "core/compositor.h"
#include "core/transport.h"
#include "hook/input_backend.h"
#include "hook/wayland_internal.h"

extern _Atomic int g_webview_dead;

static idk_transport_t g_input_tp;
static pthread_t g_accept_thread;
static pthread_mutex_t g_input_tp_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_input_inited = 0;
static _Atomic int g_accept_stop = 0;
static pthread_mutex_t g_cursor_lock = PTHREAD_MUTEX_INITIALIZER;
static idk_cursor_update_t g_cursor;
static uint8_t g_cursor_pixels[IDK_CURSOR_MAX_BYTES];
static _Atomic uint32_t g_cursor_generation = 0;

int g_input_listen_fd = -1;
int g_client_fd = -1;
int g_accept_thread_started = 0;

static void publish_cursor(const idk_cursor_update_t *cursor, const uint8_t *pixels) {
  pthread_mutex_lock(&g_cursor_lock);
  g_cursor = *cursor;
  if (cursor->data_size > 0)
    memcpy(g_cursor_pixels, pixels, cursor->data_size);
  uint32_t generation = atomic_load_explicit(&g_cursor_generation, memory_order_relaxed) + 1;
  if (generation == 0)
    generation = 1;
  atomic_store_explicit(&g_cursor_generation, generation, memory_order_release);
  pthread_mutex_unlock(&g_cursor_lock);
}

int idk_input_cursor_snapshot(uint32_t known_generation, idk_cursor_update_t *cursor, uint8_t *pixels, size_t capacity,
                              uint32_t *generation) {
  uint32_t current = atomic_load_explicit(&g_cursor_generation, memory_order_acquire);
  if (current == 0 || current == known_generation)
    return 0;
  pthread_mutex_lock(&g_cursor_lock);
  current = atomic_load_explicit(&g_cursor_generation, memory_order_relaxed);
  if (current == 0 || current == known_generation || g_cursor.data_size > capacity ||
      (g_cursor.data_size > 0 && !pixels)) {
    pthread_mutex_unlock(&g_cursor_lock);
    return 0;
  }
  *cursor = g_cursor;
  if (g_cursor.data_size > 0)
    memcpy(pixels, g_cursor_pixels, g_cursor.data_size);
  *generation = current;
  pthread_mutex_unlock(&g_cursor_lock);
  return 1;
}

extern void idk_wayland_cursor_dispatch(void);
extern void idk_x11_cursor_dispatch(void);

void idk_input_cursor_dispatch(void) {
  idk_wayland_cursor_dispatch();
  idk_x11_cursor_dispatch();
}

static void *accept_thread_main(void *arg) {
  (void)arg;
  uint8_t *pixels = malloc(IDK_CURSOR_MAX_BYTES);
  if (!pixels)
    return NULL;
  while (!atomic_load(&g_accept_stop)) {
    int rc = 0;
    int connected = 0;
    pthread_mutex_lock(&g_input_tp_lock);
    if (!g_input_tp.ready)
      connected = idk_tp_accept(&g_input_tp);
    if (g_input_tp.ready) {
      idk_cursor_update_t cursor;
      rc = idk_tp_recv_cursor(&g_input_tp, &cursor, pixels, IDK_CURSOR_MAX_BYTES);
      if (rc < 0) {
        int save_errno = errno;
        idk_tp_disconnect_client(&g_input_tp);
        WLOG("cursor receive disconnected (errno=%d)", save_errno);
      } else if (rc == 1) {
        publish_cursor(&cursor, pixels);
      }
    }
    pthread_mutex_unlock(&g_input_tp_lock);
    if (connected == 1)
      WLOG("webview connected to input transport");
    if (connected < 0) {
      WLOG("accept_thread_main: idk_tp_accept fatal - input thread exiting");
      break;
    }
    for (int i = 0; i < 20 && !atomic_load(&g_accept_stop); i++)
      usleep(100);
  }
  free(pixels);
  return NULL;
}

int init_input_socket(void) {
  if (g_input_inited) {
    WLOG("input transport already initialized - sharing");
    return 0;
  }

  char path[256];
  bool abstract = false;
  const char *abstr = getenv("IDK_INPUT_ABSTRACT");
  if (abstr && abstr[0]) {
    path[0] = '\0';
    size_t n = snprintf(path + 1, sizeof(path) - 1, "%s", abstr);
    if (n + 1 >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
      WERR("input abstract name too long: %s", abstr);
      return -1;
    }
    abstract = true;
  } else {
    const char *base = getenv("IDK_SOCKET");
    if (base && base[0]) {
      snprintf(path, sizeof(path), "%s-input", base);
    } else {
      idk_comp_get_default_socket_path(path, sizeof(path), 1);
    }
    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
      WERR("input socket path too long (%zu >= %zu): %s", strlen(path), sizeof(((struct sockaddr_un *)0)->sun_path),
           path);
      return -1;
    }
  }

  if (idk_tp_init(&g_input_tp, IDK_TP_CONSUMER, path) != 0) {
    WERR("input transport init failed for %s", path);
    return -1;
  }

  g_input_inited = 1;
  g_input_listen_fd = 0;
  g_accept_thread_started = 1;

  if (pthread_create(&g_accept_thread, NULL, accept_thread_main, NULL) != 0) {
    WERR("pthread_create failed");
    idk_tp_destroy(&g_input_tp);
    g_input_inited = 0;
    g_input_listen_fd = -1;
    g_accept_thread_started = 0;
    return -1;
  }

  WLOG("input transport listening on %s%s (backend=%s)", abstract ? "\\0" : "", abstract ? path + 1 : path,
       g_input_tp.backend == IDK_TP_SHM ? "shm" : "socket");
  return 0;
}

void send_event_to_webview(const idk_input_event_t *ev) {
  if (g_webview_dead)
    return;
  pthread_mutex_lock(&g_input_tp_lock);
  if (!g_input_tp.ready) {
    idk_tp_accept(&g_input_tp);
  }
  if (!g_input_tp.ready) {
    pthread_mutex_unlock(&g_input_tp_lock);
    return;
  }
  int rc = idk_tp_send_input(&g_input_tp, ev);
  if (rc != 0) {
    int save_errno = errno;
    bool dead = (save_errno == EPIPE || save_errno == ECONNRESET || save_errno == ESHUTDOWN ||
                 save_errno == ECONNABORTED || save_errno == EBADF);
    if (dead) {
      idk_tp_disconnect_client(&g_input_tp);
      WLOG("send_event_to_webview: webview disconnected (errno=%d)", save_errno);
    } else if (save_errno != EAGAIN) {
      WLOG("send_event_to_webview: send failed (rc=%d, errno=%d)", rc, save_errno);
    }
  }
  pthread_mutex_unlock(&g_input_tp_lock);
}

void send_overlay_state(uint8_t visible) {
  idk_input_event_t ev = {0};
  ev.type = IDK_INPUT_OVERLAY;
  ev.u.overlay.visible = visible ? 1 : 0;
  IDK_LOG("wl-input", "send_overlay_state(%u) - sending to webview\n", visible);
  send_event_to_webview(&ev);
}

void send_capture_state(uint32_t capture) {
  idk_input_event_t ev = {0};
  ev.type = IDK_INPUT_STATE;
  ev.flags = capture ? IDK_INPUT_FLAG_CAPTURE : 0;
  ev.mods = (uint16_t)g_mods;
  IDK_LOG("wl-input", "send_capture_state(%u) flags=0x%x mods=0x%x - sending to webview\n", capture, ev.flags, ev.mods);
  send_event_to_webview(&ev);
}

void send_repeat_info(void) {
  idk_input_event_t ev = {0};
  ev.type = IDK_INPUT_REPEAT;
  ev.flags = IDK_INPUT_FLAG_CAPTURE;
  ev.u.repeat.rate = (uint16_t)g_repeat_rate;
  ev.u.repeat.delay = (uint16_t)g_repeat_delay;
  send_event_to_webview(&ev);
}

void teardown_input_socket(void) {
  if (g_accept_thread_started) {
    atomic_store(&g_accept_stop, 1);
    pthread_mutex_lock(&g_input_tp_lock);
    idk_tp_disconnect_client(&g_input_tp);
    pthread_mutex_unlock(&g_input_tp_lock);
    pthread_join(g_accept_thread, NULL);
    g_accept_thread_started = 0;
    atomic_store(&g_accept_stop, 0);
  }

  pthread_mutex_lock(&g_input_tp_lock);
  idk_tp_destroy(&g_input_tp);
  g_input_inited = 0;
  g_input_listen_fd = -1;
  atomic_store_explicit(&g_cursor_generation, 0, memory_order_release);
  pthread_mutex_unlock(&g_input_tp_lock);
}
