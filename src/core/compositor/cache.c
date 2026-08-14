#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/compositor.h"
#include "core/log.h"

/* ===== SHM mmap cache ===== */

void *idk_shm_cache_map(idk_shm_cache_t *c, int fd) {
  if (fd < 0)
    return NULL;
  struct stat st;
  if (fstat(fd, &st) < 0)
    return NULL;

  if (c->map && st.st_ino == c->ino && st.st_dev == c->dev)
    return c->map;

  if (c->map) {
    munmap(c->map, c->size);
    c->map = NULL;
  }

  off_t total = lseek(fd, 0, SEEK_END);
  if (total <= 0)
    total = 4096;
  c->size = (size_t)total;
  c->map = mmap(NULL, c->size, PROT_READ, MAP_SHARED, fd, 0);
  if (c->map == MAP_FAILED) {
    c->map = NULL;
    return NULL;
  }
  c->ino = st.st_ino;
  c->dev = st.st_dev;
  return c->map;
}

void idk_shm_cache_cleanup(idk_shm_cache_t *c) {
  if (c->map) {
    munmap(c->map, c->size);
    c->map = NULL;
  }
  c->size = 0;
  c->ino = 0;
  c->dev = 0;
}
