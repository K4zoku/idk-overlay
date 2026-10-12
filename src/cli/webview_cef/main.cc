/* idk-webview-cef - CEF (Chromium) webview for idk-overlay.
 *
 * Off-screen windowless browser with shared textures; frames are handed to
 * the game's compositor as DMA-BUF fds (SHM fallback on rejection). Input
 * comes from the game's wayland hook over the idk input socket.
 *
 * Environment:
 *   IDK_TP_ABSTRACT     abstract producer socket (set by broker)
 *   IDK_INPUT_ABSTRACT  abstract input socket (set by broker)
 *   IDK_SOCKET          filesystem producer socket fallback
 *   IDK_TP_SOCKET       seqpacket transport opt-in (must match compositor)
 *   IDK_GPU_SYNC        Vulkan DMA-BUF release synchronization opt-in
 *   IDK_MATCH           process name (when not given via --match)
 *   IDK_CEF_DIR         CEF dist root (default: compiled-in CEF_DIST_DIR)
 *   IDK_CEF_DEBUG_PORT  enable DevTools on this port (optional)
 */
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "app.h"
#include "config.h"
#include "core/log.h"
#include "include/capi/cef_app_capi.h"
#include "include/cef_version_info.h"
#include "public/idk_producer.h"
#include "tasks.h"
#include "view.h"

#ifndef CEF_DIST_DIR
#define CEF_DIST_DIR ""
#endif

/* CLI options mirror the Qt webview: [config] [--config] [--socket]
 * [--no-dmabuf] [--url] [--width] [--height] [--match]. */
static const char *ArgValue(int argc, char **argv, const char *name, const char **out) {
  for (int i = 1; i < argc - 1; i++) {
    if (strcmp(argv[i], name) == 0) {
      *out = argv[i + 1];
      return argv[i + 1];
    }
  }
  return nullptr;
}


static bool HasFlag(int argc, char **argv, const char *name) {
  for (int i = 1; i < argc; i++)
    if (strcmp(argv[i], name) == 0)
      return true;
  return false;
}

/* Same default as the Qt webview: $XDG_CONFIG_HOME/idk-overlay.conf. */
static std::string DefaultConfigPath() {
  const char *xdg = getenv("XDG_CONFIG_HOME");
  if (xdg && *xdg)
    return std::string(xdg) + "/idk-overlay.conf";
  const char *home = getenv("HOME");
  return std::string(home ? home : "") + "/.config/idk-overlay.conf";
}

/* Resolve the producer socket. Order: IDK_TP_ABSTRACT > --socket >
 * IDK_SOCKET. Returns false when nothing is set (must be launched by the
 * game hook, like the Qt webview). */
static bool ResolveSocket(int argc, char **argv, std::string *path, bool *abstract) {
  const char *env = getenv("IDK_TP_ABSTRACT");
  if (env && *env) {
    *path = env;
    *abstract = true;
    return true;
  }
  const char *cli = nullptr;
  ArgValue(argc, argv, "--socket", &cli);
  if (cli && *cli) {
    *path = cli;
    *abstract = false;
    return true;
  }
  env = getenv("IDK_SOCKET");
  if (env && *env) {
    *path = env;
    *abstract = false;
    return true;
  }
  return false;
}

/* Chromium loads icudtl.dat from DIR_ASSETS, which CEF overrides to the
 * directory containing libcef.so. Copy it there once if missing. */
static void EnsureIcuData(const std::string &resources_dir) {
  Dl_info info;
  const char *libcef_dir = nullptr;
  if (dladdr((void *)&cef_initialize, &info) && info.dli_fname)
    libcef_dir = info.dli_fname;

  std::string target_dir;
  if (libcef_dir) {
    std::string f = libcef_dir;
    target_dir = f.substr(0, f.find_last_of('/'));
  } else {
    target_dir = std::string(CEF_DIST_DIR) + "/Release";
  }
  if (target_dir.empty())
    return;

  std::string target = target_dir + "/icudtl.dat";
  if (access(target.c_str(), R_OK) == 0)
    return;

  std::string src = resources_dir + "/icudtl.dat";
  std::ifstream in(src, std::ios::binary);
  if (!in) {
    IDK_LOG("webview-cef", "icudtl.dat not found in %s\n", src.c_str());
    return;
  }
  std::ofstream out(target, std::ios::binary);
  out << in.rdbuf();
  IDK_LOG("webview-cef", "copied icudtl.dat -> %s\n", target.c_str());
}

int main(int argc, char *argv[]) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  CefMainArgs main_args(argc, argv);
  CefRefPtr<App> app(new App);

  /* Secondary processes (renderer, GPU, zygote...) are re-execs of this
   * binary with --type=...; CefExecuteProcess handles them entirely. */
  int exit_code = CefExecuteProcess(main_args, app, nullptr);
  if (exit_code >= 0)
    return exit_code;

  bool no_dmabuf = HasFlag(argc, argv, "--no-dmabuf");
  const char *cli_url = nullptr, *cli_match = nullptr, *cli_w = nullptr, *cli_h = nullptr;
  ArgValue(argc, argv, "--url", &cli_url);
  ArgValue(argc, argv, "--match", &cli_match);
  ArgValue(argc, argv, "--width", &cli_w);
  ArgValue(argc, argv, "--height", &cli_h);
  const char *conf_path = FindConfigPath(argc, argv);

  std::string sock_path;
  bool sock_abstract = false;
  if (!ResolveSocket(argc, argv, &sock_path, &sock_abstract)) {
    IDK_LOG("webview-cef", "no socket configured (IDK_TP_ABSTRACT/IDK_SOCKET/"
                           "--socket) - must be launched by the game hook\n");
    return 1;
  }

  /* View config: CLI --url wins; otherwise parse the INI config and pick
   * the section matching the process name. */
  GroupConfig conf;
  if (cli_url && *cli_url) {
    conf.url = cli_url;
    if (cli_w && atoi(cli_w) > 0)
      conf.width = atoi(cli_w);
    if (cli_h && atoi(cli_h) > 0)
      conf.height = atoi(cli_h);
  } else {
    std::string proc = cli_match ? cli_match : "";
    if (proc.empty()) {
      const char *env = getenv("IDK_MATCH");
      if (env && *env)
        proc = env;
    }
    std::vector<GroupConfig> groups = conf_path ? ParseConfig(conf_path) : ParseConfig(DefaultConfigPath());
    if (!SelectView(groups, proc, &conf)) {
      IDK_LOG("webview-cef", "no config section for '%s' (config: %s)\n", proc.c_str(),
              conf_path ? conf_path : "(default)");
      return 0; /* exit(0) → hook treats as user-close, overlay disabled */
    }
  }

  const char *cef_dir = getenv("IDK_CEF_DIR");
  if (!cef_dir || !*cef_dir)
    cef_dir = CEF_DIST_DIR;
  std::string resources = std::string(cef_dir) + "/Resources";
  EnsureIcuData(resources);

  CefSettings settings; /* wrapper ctor sets size; do NOT memset */
  settings.no_sandbox = true;
  settings.windowless_rendering_enabled = true;
  settings.external_message_pump = true;
  settings.background_color = 0; /* fully transparent painting */
  settings.log_severity = LOGSEVERITY_WARNING;
  CefString(&settings.resources_dir_path).FromASCII(resources.c_str());
  CefString(&settings.locales_dir_path).FromASCII((resources + "/locales").c_str());
  const char *rt = getenv("XDG_RUNTIME_DIR");
  /* Per-pid cache: CEF's process-singleton locks the cache dir, so a
   * lingering webview (crashed game, leftover child) would block the
   * next one. An overlay needs no persistent profile. */
  std::string cache = std::string(rt && *rt ? rt : "/tmp") + "/idk-webview-cef-cache-" + std::to_string((long)getpid());
  CefString(&settings.cache_path).FromASCII(cache.c_str());
  const char *dbg = getenv("IDK_CEF_DEBUG_PORT");
  if (dbg && *dbg)
    settings.remote_debugging_port = atoi(dbg);

  CefPump pump;
  if (!pump.Valid()) {
    IDK_LOG("webview-cef", "CEF message pump initialization failed\n");
    return 1;
  }
  g_cef_pump.store(&pump, std::memory_order_release);

  if (!CefInitialize(main_args, settings, app, nullptr)) {
    IDK_LOG("webview-cef", "CefInitialize failed\n");
    return 1;
  }
  IDK_LOG("webview-cef", "CEF %d.%d.%d, Chromium %d.%d.%d.%d\n", cef_version_info(0), cef_version_info(1),
          cef_version_info(2), cef_version_info(3), cef_version_info(4), cef_version_info(5), cef_version_info(6),
          cef_version_info(7));

  CefRefPtr<View> view(new View(std::move(conf), no_dmabuf, sock_path, sock_abstract));

  CefWindowInfo wi;
  wi.SetAsWindowless(0);
  wi.shared_texture_enabled = true; /* dmabuf planes via OnAcceleratedPaint */
  /* Game-driven rendering: begin frames are requested by the game, while
   * Chromium's video capturer delivers paints only when the surface changes.
   * A BeginFrame can complete without producing any paint callback. */
  wi.external_begin_frame_enabled = true;

  CefBrowserSettings bs; /* wrapper ctor sets size */
  bs.background_color = 0;
  /* CEF defaults windowless OSR to 30 fps. External begin frames still pass
   * through this cap, so leave headroom for high-refresh game requests. */
  bs.windowless_frame_rate = 1000;

  CefRefPtr<CefBrowser> browser = CefBrowserHost::CreateBrowserSync(wi, view, "about:blank", bs, nullptr, nullptr);
  if (!browser) {
    IDK_LOG("webview-cef", "CreateBrowserSync failed\n");
    CefShutdown();
    return 1;
  }

  view->Start(); /* producer connect + input thread */

  pump.Schedule(0);
  CefDoMessageLoopWork();
  for (;;) {
    if (view->QuitRequested())
      break;

    pump.Reconcile();
    const int producer_fd = idk_producer_poll_fd();
    struct pollfd pfds[3] = {
        {.fd = producer_fd, .events = producer_fd >= 0 ? POLLIN : 0, .revents = 0},
        {.fd = pump.WakeFd(), .events = POLLIN, .revents = 0},
        {.fd = pump.TimerFd(), .events = POLLIN, .revents = 0},
    };
    int rc = poll(pfds, 3, 8);
    if (rc < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (pfds[1].revents & POLLIN) {
      pump.DrainWake();
      pump.Reconcile();
    }
    if (pfds[2].revents & POLLIN) {
      pump.DrainTimer();
      pump.Reconcile();
    }
    if (producer_fd >= 0 && pfds[0].revents & (POLLIN | POLLHUP | POLLERR))
      view->PollSocket();
    CefDoMessageLoopWork();
  }
  view->Stop(); /* join the input thread */
  view = nullptr;

  /* No CefShutdown: pending tasks still hold refs to the View (which CEF's
   * shutdown checker treats as a FATAL), and windowless teardown crashes.
   * The process is per-game — the hook monitor / broker SIGTERM it on game
   * exit, so process exit is the correct teardown. */
  _exit(0);
}
