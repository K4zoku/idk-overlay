#include <string.h>

#include "core/log.h"

#include "internal.h"

IDK_INTERNAL idk_hook_plugin_t *g_plugins[] = {
    &idk_plugin_egl,
    &idk_plugin_glx,
};

IDK_INTERNAL int g_hooks_installed = 0;
IDK_INTERNAL int g_egl_hook_installed = 0;

IDK_INTERNAL int plugins_count(void) { return (int)(sizeof(g_plugins) / sizeof(g_plugins[0])); }

/* One install pass over all enabled plugins; retried from the poll loop. */
IDK_INTERNAL void plugins_install_try(int *done, int n_plugins) {
  for (int p = 0; p < n_plugins; p++) {
    if (done[p])
      continue;
    idk_hook_plugin_t *plug = g_plugins[p];
    int enabled = (strcmp(plug->name, "vk-syringe") == 0) ? g_enable_vk : g_enable_gl;
    if (!enabled) {
      done[p] = 1;
      continue;
    }
    if (strcmp(plug->name, "egl") == 0 && g_hooks_installed && !g_egl_hook_installed) {
      done[p] = 1;
      continue;
    }
    if (plugin_lib_loaded(plug)) {
      IDK_LOG("overlay", "%s: library found, installing hook\n", plug->name);
      int r = plug->init();
      if (r == 0) {
        done[p] = 1;
        g_hooks_installed = 1;
        if (strcmp(plug->name, "egl") == 0)
          g_egl_hook_installed = 1;
        IDK_LOG("overlay", "%s: hook installed OK\n", plug->name);
      } else {
        IDK_LOG("overlay", "%s: hook install failed, will retry\n", plug->name);
      }
    }
  }
}
