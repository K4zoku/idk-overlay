/* import_gl.c - dmabuf import via GL_EXT_memory_object (MangoHud approach).
 * Pass dmabuf fds to glImportMemoryFdEXT with GL_HANDLE_TYPE_OPAQUE_FD_EXT.
 * GLX path - no EGL needed. */

#include <dlfcn.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "internal.h"

#define GL_HANDLE_TYPE_OPAQUE_FD_EXT 0x9586
#define GL_TEXTURE_SWIZZLE_R 0x8E42
#define GL_TEXTURE_SWIZZLE_G 0x8E43
#define GL_TEXTURE_SWIZZLE_B 0x8E44
#define GL_TEXTURE_SWIZZLE_A 0x8E45
#define GL_ALPHA_ENUM 0x1906
#define GL_BLUE 0x1905
#define GL_GREEN 0x1904
#define GL_RED 0x1903

typedef unsigned long long GLu64;
typedef void (*PFN_glCreateMemoryObjectsEXT_fn)(GLsizei, GLuint *);
typedef void (*PFN_glDeleteMemoryObjectsEXT_fn)(GLsizei, const GLuint *);
typedef void (*PFN_glImportMemoryFdEXT_fn)(GLuint, GLu64, GLenum, GLint);
typedef void (*PFN_glTexStorageMem2DEXT_fn)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLu64);

static PFN_glCreateMemoryObjectsEXT_fn fn_glCreateMemoryObjectsEXT = NULL;
static PFN_glDeleteMemoryObjectsEXT_fn fn_glDeleteMemoryObjectsEXT = NULL;
static PFN_glImportMemoryFdEXT_fn fn_glImportMemoryFdEXT = NULL;
static PFN_glTexStorageMem2DEXT_fn fn_glTexStorageMem2DEXT = NULL;
static int g_gl_mem_resolved = 0;
static int g_gl_mem_available = 0;

static void resolve_gl_memory_functions(void) {
  if (g_gl_mem_resolved)
    return;
  g_gl_mem_resolved = 1;

  if (!idk_fn_glGetString)
    return;
  const GLubyte *exts = idk_fn_glGetString(0x1F03);
  if (!exts)
    return;

  if (!strstr((const char *)exts, "GL_EXT_memory_object") || !strstr((const char *)exts, "GL_EXT_memory_object_fd")) {
    IDK_LOG("comp", "GL_EXT_memory_object(_fd) not available\n");
    return;
  }

  void *lib = real_dlopen("libGL.so.1", RTLD_NOW | RTLD_NOLOAD);
  if (!lib)
    lib = real_dlopen("libGL.so", RTLD_NOW | RTLD_NOLOAD);
  if (!lib)
    return;

  fn_glCreateMemoryObjectsEXT = (PFN_glCreateMemoryObjectsEXT_fn)real_dlsym(lib, "glCreateMemoryObjectsEXT");
  fn_glDeleteMemoryObjectsEXT = (PFN_glDeleteMemoryObjectsEXT_fn)real_dlsym(lib, "glDeleteMemoryObjectsEXT");
  fn_glImportMemoryFdEXT = (PFN_glImportMemoryFdEXT_fn)real_dlsym(lib, "glImportMemoryFdEXT");
  fn_glTexStorageMem2DEXT = (PFN_glTexStorageMem2DEXT_fn)real_dlsym(lib, "glTexStorageMem2DEXT");

  if (fn_glCreateMemoryObjectsEXT && fn_glImportMemoryFdEXT && fn_glTexStorageMem2DEXT) {
    g_gl_mem_available = 1;
    IDK_LOG("comp", "GL_EXT_memory_object: available (MangoHud-style dmabuf import)\n");
  } else {
    IDK_LOG("comp", "GL_EXT_memory_object: extension present but functions not resolved\n");
  }
}

/* dup() the fd and import. glImportMemoryFdEXT takes ownership of the
 * passed fd - after a successful import we must NOT close it. Uses
 * stride*height as the import size (lseek(SEEK_END) returns the gbm
 * allocation size, 2MiB-aligned and larger than stride*h). */
static int gl_import_try(GLuint mem, int dmabuf_fd, GLu64 size, uint32_t stride, uint32_t h) {
  int import_fd = dup(dmabuf_fd);
  if (import_fd < 0) {
    IDK_ERR("comp", "dup(dmabuf_fd=%d) failed: %s\n", dmabuf_fd, strerror(errno));
    fn_glDeleteMemoryObjectsEXT(1, &mem);
    return 0;
  }

  fn_glImportMemoryFdEXT(mem, size, GL_HANDLE_TYPE_OPAQUE_FD_EXT, import_fd);
  GLenum err = idk_fn_glGetError ? idk_fn_glGetError() : GL_NO_ERROR;
  if (err != GL_NO_ERROR) {
    IDK_ERR("comp", "glImportMemoryFdEXT failed: 0x%04x (fd=%d size=%llu stride=%u h=%u)\n", err, import_fd,
            (unsigned long long)size, stride, (unsigned int)h);
    close(import_fd);
    fn_glDeleteMemoryObjectsEXT(1, &mem);
    return 0;
  }
  return 1;
}

/* Create the texture backed by the memory object, apply the format
 * swizzle and filtering. Deletes mem on success (the texture retains
 * its own reference to the imported memory) and on failure. */
static GLuint gl_texture_create(GLuint mem, uint32_t w, uint32_t h, uint32_t fourcc) {
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);

  fn_glTexStorageMem2DEXT(GL_TEXTURE_2D, 1, GL_RGBA8, w, h, mem, 0);

  GLenum err = idk_fn_glGetError ? idk_fn_glGetError() : GL_NO_ERROR;
  if (err != GL_NO_ERROR) {
    IDK_ERR("comp", "glTexStorageMem2DEXT failed: 0x%04x (w=%u h=%u)\n", err, w, h);
    glDeleteTextures(1, &tex);
    fn_glDeleteMemoryObjectsEXT(1, &mem);
    return 0;
  }

  if (fourcc == 0x34324142) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_GREEN);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_BLUE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_ALPHA_ENUM);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_RED);
  }

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);

  glBindTexture(GL_TEXTURE_2D, 0);

  fn_glDeleteMemoryObjectsEXT(1, &mem);
  return tex;
}

/* Import dmabuf as GL texture via GL_EXT_memory_object (MangoHud approach).
 * Returns texture ID on success, 0 on failure. Caller keeps fd ownership -
 * the GL driver takes ownership of the dup'd fd; the caller's original fd
 * must be kept alive separately (tracked in g_tex_dmabuf_fd[] for cache
 * invalidation on resize). */
GLuint gl_dmabuf_to_texture(int dmabuf_fd, uint32_t w, uint32_t h, uint32_t stride, uint32_t fourcc) {
  resolve_gl_memory_functions();
  if (!g_gl_mem_available)
    return 0;

  if (idk_fn_glGetError) {
    GLenum e;
    int drains = 0;
    while ((e = idk_fn_glGetError()) != GL_NO_ERROR && drains < 10)
      drains++;
    if (drains > 0) {
      static int s_drain_logged = 0;
      if (!s_drain_logged) {
        IDK_LOG("comp", "gl_dmabuf_to_texture: drained %d stale GL errors\n", drains);
        s_drain_logged = 1;
      }
    }
  }

  GLuint mem = 0;
  fn_glCreateMemoryObjectsEXT(1, &mem);
  if (idk_fn_glGetError) {
    while (idk_fn_glGetError() != GL_NO_ERROR)
      ;
  }
  if (!mem) {
    IDK_ERR("comp", "glCreateMemoryObjectsEXT returned 0\n");
    return 0;
  }

  GLu64 size = (GLu64)stride * (GLu64)h;
  if (!gl_import_try(mem, dmabuf_fd, size, stride, h))
    return 0;

  GLuint tex = gl_texture_create(mem, w, h, fourcc);
  if (tex == 0)
    return 0;

  IDK_LOG("comp", "GL dmabuf import OK (MangoHud-style): %ux%u tex=%u fourcc=0x%x size=%llu\n", w, h, tex, fourcc,
          (unsigned long long)size);
  return tex;
}
