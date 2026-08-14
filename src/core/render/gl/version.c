#include <stdio.h>
#include <string.h>

#include "core/log.h"
#include "gl/gl_loader.h"

#include "internal.h"

/* Detect GL version from GL_VERSION string */

void detect_gl_version(int *out_gl_version, bool *out_is_gles) {
  const char *version = (const char *)glGetString(GL_VERSION);
  int gl_version = 0;
  bool is_gles = false;

  if (version) {
    const char *es_prefixes[] = {"OpenGL ES-CM ", "OpenGL ES-CL ", "OpenGL ES ", NULL};
    for (int i = 0; es_prefixes[i]; i++) {
      size_t plen = strlen(es_prefixes[i]);
      if (strncmp(version, es_prefixes[i], plen) == 0) {
        version += plen;
        is_gles = true;
        break;
      }
    }
    int major = 0, minor = 0;
    sscanf(version, "%d.%d", &major, &minor);
    gl_version = major * 100 + minor * 10;
    if (is_gles && gl_version < 300)
      gl_version = 200;
    IDK_LOG("shdr", "GL version: %d.%d %s (g_gl_version=%d)\n", major, minor, is_gles ? "ES" : "", gl_version);
  }

  *out_gl_version = gl_version;
  *out_is_gles = is_gles;
}
