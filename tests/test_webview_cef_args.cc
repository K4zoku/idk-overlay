#include <cassert>
#include <cstring>

#include "../src/cli/webview_cef/config.h"

int main() {
  char program[] = "idk-webview-cef";
  char socket_opt[] = "--socket";
  char socket_path[] = "/run/user/1000/idk-overlay-443211";
  char *socket_argv[] = {program, socket_opt, socket_path};
  assert(FindConfigPath(3, socket_argv) == nullptr);

  char config_opt[] = "--config";
  char config_path[] = "/tmp/idk-overlay.conf";
  char match_opt[] = "--match";
  char match[] = "osu!";
  char *explicit_argv[] = {program, socket_opt, socket_path, match_opt, match, config_opt, config_path};
  assert(std::strcmp(FindConfigPath(7, explicit_argv), config_path) == 0);

  char *positional_argv[] = {program, socket_opt, socket_path, match_opt, match, config_path};
  assert(std::strcmp(FindConfigPath(6, positional_argv), config_path) == 0);
  return 0;
}
