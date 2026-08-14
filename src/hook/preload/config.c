#include <dlfcn.h>
#include <limits.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/log.h"
#include "public/idk_input.h"

#include "internal.h"

typedef unsigned long KeySym;

static void get_config_path(char *buf, size_t bufsz) {
  const char *env = getenv("IDK_CONFIG");
  if (env && env[0]) {
    snprintf(buf, bufsz, "%s", env);
    return;
  }
  const char *home = getenv("HOME");
  if (!home)
    home = "/tmp";
  const char *xdg = getenv("XDG_CONFIG_HOME");
  if (xdg && xdg[0])
    snprintf(buf, bufsz, "%s/idk-overlay.conf", xdg);
  else
    snprintf(buf, bufsz, "%s/.config/idk-overlay.conf", home);
}

static void parse_hotkey_str(const char *str, uint32_t *keysym, uint32_t *mods) {
  *keysym = 0;
  *mods = 0;
  if (!str || !str[0])
    return;
  const char *keyname = str;
  const char *plus = strchr(str, '+');
  if (plus && plus > str) {
    size_t n = (size_t)(plus - str);
    char mod[32];
    if (n < sizeof(mod)) {
      memcpy(mod, str, n);
      mod[n] = '\0';
      if (strcasecmp(mod, "Shift") == 0)
        *mods = IDK_MOD_SHIFT;
      else if (strcasecmp(mod, "Ctrl") == 0)
        *mods = IDK_MOD_CTRL;
      else if (strcasecmp(mod, "Alt") == 0)
        *mods = IDK_MOD_ALT;
      else if (strcasecmp(mod, "Super") == 0)
        *mods = IDK_MOD_SUPER;
    }
    keyname = plus + 1;
  }
  static KeySym (*p_xstr)(const char *) = NULL;
  static int tried = 0;
  if (!tried) {
    tried = 1;
    void *lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_NOLOAD);
    if (!lib)
      lib = dlopen("libX11.so.6", RTLD_NOW);
    if (lib)
      p_xstr = (KeySym (*)(const char *))dlsym(lib, "XStringToKeysym");
  }
  if (p_xstr) {
    KeySym ks = p_xstr(keyname);
    if (ks)
      *keysym = (uint32_t)ks;
  }
  if (!*keysym) {
    if (!strcasecmp(keyname, "Tab"))
      *keysym = 0xff09;
    else if (!strcasecmp(keyname, "F8"))
      *keysym = 0xffc5;
    else if (!strcasecmp(keyname, "F9"))
      *keysym = 0xffc6;
    else if (!strcasecmp(keyname, "F10"))
      *keysym = 0xffc7;
    else if (!strcasecmp(keyname, "F11"))
      *keysym = 0xffc8;
    else if (!strcasecmp(keyname, "F12"))
      *keysym = 0xffc9;
  }
}

IDK_INTERNAL void load_hotkey_config(void) {
  const char *env_cap = getenv("IDK_HOTKEY_CAPTURE");
  if (!env_cap || !env_cap[0])
    env_cap = "Shift+Tab";
  const char *env_ovl = getenv("IDK_HOTKEY_OVERLAY");
  if (!env_ovl || !env_ovl[0])
    env_ovl = "F8";
  parse_hotkey_str(env_cap, &g_hotkey_keysym, &g_hotkey_mods);
  parse_hotkey_str(env_ovl, &g_hotkey_overlay_keysym, &g_hotkey_overlay_mods);
  const char *proc = idk_process_name();
  char cpath[PATH_MAX];
  get_config_path(cpath, sizeof(cpath));
  FILE *f = fopen(cpath, "r");
  if (!f) {
    IDK_LOG("overlay", "hotkey: using defaults (cap=%s ovl=%s)\n", env_cap, env_ovl);
    return;
  }
  char line[512], cur_section[128] = {0};
  int in_match = 0;
  char found_cap[128] = {0}, found_ovl[128] = {0};
  while (fgets(line, sizeof(line), f)) {
    char *s = line;
    while (*s == ' ' || *s == '\t')
      s++;
    char *e = s + strlen(s) - 1;
    while (e > s && (*e == '\n' || *e == '\r' || *e == ' '))
      *e-- = '\0';
    if (*s == '#' || *s == ';' || !*s)
      continue;
    if (*s == '[') {
      char *c = strchr(s, ']');
      if (c) {
        *c = '\0';
        snprintf(cur_section, sizeof(cur_section), "%s", s + 1);
        in_match = 0;
      }
      continue;
    }
    char *eq = strchr(s, '=');
    if (!eq)
      continue;
    *eq = '\0';
    char *v = eq + 1;
    while (*v == ' ')
      v++;
    if (!strcasecmp(s, "Match")) {
      if (proc[0] && v[0]) {
        regex_t re;
        if (regcomp(&re, v, REG_NOSUB | REG_EXTENDED) == 0) {
          if (regexec(&re, proc, 0, NULL, 0) == 0)
            in_match = 1;
          regfree(&re);
        }
      }
    } else if (in_match) {
      if (!strcasecmp(s, "HotkeyCapture"))
        snprintf(found_cap, sizeof(found_cap), "%s", v);
      else if (!strcasecmp(s, "HotkeyOverlay"))
        snprintf(found_ovl, sizeof(found_ovl), "%s", v);
    }
  }
  fclose(f);
  if (found_cap[0]) {
    parse_hotkey_str(found_cap, &g_hotkey_keysym, &g_hotkey_mods);
    IDK_LOG("overlay", "hotkey: capture='%s' from config matching '%s'\n", found_cap, proc);
  }
  if (found_ovl[0]) {
    parse_hotkey_str(found_ovl, &g_hotkey_overlay_keysym, &g_hotkey_overlay_mods);
    IDK_LOG("overlay", "hotkey: overlay='%s' from config matching '%s'\n", found_ovl, proc);
  }
}
