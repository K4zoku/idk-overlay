#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include "internal.h"

static int maps_contains(const char *substr) {
  FILE *f = fopen("/proc/self/maps", "r");
  if (!f)
    return 0;
  char line[1024];
  int found = 0;
  while (!found && fgets(line, sizeof(line), f)) {
    if (strstr(line, substr))
      found = 1;
  }
  fclose(f);
  return found;
}

IDK_INTERNAL int lib_loaded(const char *name) {
  void *h = dlopen(name, RTLD_NOW | RTLD_NOLOAD);
  if (h) {
    dlclose(h);
    return 1;
  }
  return maps_contains(name);
}

IDK_INTERNAL int plugin_lib_loaded(const idk_hook_plugin_t *p) {
  for (int i = 0; p->lib_patterns[i]; i++) {
    if (lib_loaded(p->lib_patterns[i]))
      return 1;
  }
  return 0;
}
