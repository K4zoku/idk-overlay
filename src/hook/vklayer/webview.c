#include <errno.h>
#include <limits.h>
#include <sys/eventfd.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/prctl.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core/compositor.h"
#include "core/log.h"
#include "internal.h"

static pid_t g_webview_pid = -1;
static int g_input_eventfd = -1;

static int find_webview_bin(char *buf, size_t bufsz) {
  const char *env = getenv("IDK_WEBVIEW_BIN");
  if (env && env[0]) {
    snprintf(buf, bufsz, "%s", env);
    return 0;
  }

  const char *sel = getenv("IDK_WEBVIEW");
  if (!sel || !sel[0])
    sel = "idk-webview";
  if (strchr(sel, '/')) {
    snprintf(buf, bufsz, "%s", sel);
    return 0;
  }

  const char *path = getenv("PATH");
  if (!path)
    path = "/usr/sbin:/usr/local/bin:/usr/bin:/bin";
  const size_t sel_len = strlen(sel);
  for (const char *p = path; *p;) {
    const char *colon = strchr(p, ':');
    const size_t len = colon ? (size_t)(colon - p) : strlen(p);
    if (len > 0 && len + sel_len + 2 < bufsz) {
      snprintf(buf, bufsz, "%.*s/%s", (int)len, p, sel);
      if (access(buf, X_OK) == 0)
        return 0;
    }
    if (!colon)
      break;
    p = colon + 1;
  }
  return -1;
}

static void disable_webview(void) {
  atomic_store(&g_webview_dead, 1);
  atomic_store(&g_overlay_visible, 0);
}

static void *monitor_webview(void *arg) {
  const pid_t pid = (pid_t)(intptr_t)arg;
  int status = 0;
  if (waitpid(pid, &status, 0) <= 0 || atomic_load(&g_webview_dead))
    return NULL;
  IDK_LOG("overlay", "Vulkan layer webview exited: pid=%d status=0x%x\n", (int)pid, status);
  disable_webview();
  return NULL;
}

static char *filter_instance_layers(const char *value) {
  static const char layer_name[] = "VK_LAYER_IDK_overlay";
  const size_t layer_len = sizeof(layer_name) - 1;
  const size_t value_len = strlen(value);
  char *filtered = malloc(value_len + 1);
  if (!filtered)
    return NULL;

  size_t out_len = 0;
  const char *p = value;
  while (*p) {
    const char *end = strchr(p, ':');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len != layer_len || strncmp(p, layer_name, layer_len) != 0) {
      if (out_len > 0)
        filtered[out_len++] = ':';
      memcpy(filtered + out_len, p, len);
      out_len += len;
    }
    if (!end)
      break;
    p = end + 1;
  }
  filtered[out_len] = '\0';
  return filtered;
}

static char **build_child_env(char **owned_layers) {
  extern char **environ;
  size_t count = 0;
  while (environ[count])
    count++;

  char **envp = calloc(count + 3, sizeof(*envp));
  if (!envp)
    return NULL;

  size_t out = 0;
  int has_layers = 0;
  for (size_t i = 0; i < count; i++) {
    if ((strncmp(environ[i], "IDK_VK_LAYER=", 13) == 0 ||
         strncmp(environ[i], "IDK_VK_DISABLE=", 15) == 0) ||
        (strncmp(environ[i], "LD_PRELOAD=", 11) == 0 && strstr(environ[i] + 11, "libidk-overlay.so")))
      continue;
    if (strncmp(environ[i], "VK_INSTANCE_LAYERS=", 19) == 0) {
      char *filtered = filter_instance_layers(environ[i] + 19);
      if (!filtered) {
        free(envp);
        return NULL;
      }
      const size_t filtered_len = strlen(filtered);
      char *entry = malloc(19 + filtered_len + 1);
      if (!entry) {
        free(filtered);
        free(envp);
        return NULL;
      }
      memcpy(entry, "VK_INSTANCE_LAYERS=", 19);
      memcpy(entry + 19, filtered, filtered_len + 1);
      free(filtered);
      envp[out++] = entry;
      *owned_layers = entry;
      has_layers = 1;
      continue;
    }
    envp[out++] = environ[i];
  }
  envp[out++] = "IDK_VK_DISABLE=1";
  if (!has_layers)
    envp[out++] = "VK_INSTANCE_LAYERS=";
  envp[out] = NULL;
  return envp;
}

static void free_child_env(char **envp, char *owned_layers) {
  free(owned_layers);
  free(envp);
}

int idk_vk_layer_spawn_webview(void) {
  char bin[PATH_MAX];
  if (find_webview_bin(bin, sizeof(bin)) != 0) {
    IDK_LOG("overlay", "Vulkan layer webview not found (set IDK_WEBVIEW_BIN/IDK_WEBVIEW)\n");
    return -1;
  }

  if (g_input_eventfd < 0) {
    g_input_eventfd = eventfd(0, EFD_SEMAPHORE | EFD_NONBLOCK);
    if (g_input_eventfd >= 0) {
      char efd[16];
      snprintf(efd, sizeof(efd), "%d", g_input_eventfd);
      setenv("IDK_INPUT_EVENTFD", efd, 1);
    }
  }
  char socket_path[PATH_MAX];
  idk_comp_get_path(socket_path, sizeof(socket_path));
  const char *comm = idk_process_name();
  char *owned_layers = NULL;
  char **child_env = build_child_env(&owned_layers);
  if (!child_env) {
    IDK_ERR("overlay", "Vulkan layer webview environment allocation failed\n");
    return -1;
  }

  g_webview_pid = fork();
  if (g_webview_pid < 0) {
    free_child_env(child_env, owned_layers);
    IDK_ERR("overlay", "Vulkan layer webview fork failed: %s\n", strerror(errno));
    return -1;
  }
  if (g_webview_pid == 0) {
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    for (int fd = 3; fd < 1024; fd++) {
      if (fd == g_input_eventfd)
        continue;
      close(fd);
    }

    char *argv[6];
    int argc = 0;
    argv[argc++] = bin;
    if (socket_path[0] != '\0') {
      argv[argc++] = "--socket";
      argv[argc++] = socket_path;
    }
    argv[argc++] = "--match";
    argv[argc++] = (char *)(comm ? comm : "");
    argv[argc] = NULL;
    execve(bin, argv, child_env);
    _exit(127);
  }
  free_child_env(child_env, owned_layers);
  IDK_LOG("overlay", "Vulkan layer webview forked: pid=%d bin=%s\n", (int)g_webview_pid, bin);
  pthread_t monitor;
  if (pthread_create(&monitor, NULL, monitor_webview, (void *)(intptr_t)g_webview_pid) == 0)
    pthread_detach(monitor);
  return 0;
}
