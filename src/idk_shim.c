/*
 * idk_shim.c — dlsym interceptor for idk-overlay
 *
 * Acts as the first LD_PRELOAD library. Intercepts dlsym() to redirect
 * GL/EGL function lookups to our hooks. When Electron/Chromium calls
 * dlsym(handle, "eglSwapBuffers"), this returns our hooked version.
 *
 * Architecture (inspired by MangoHud):
 *   libidk-shim.so     — dlsym interceptor (this file, LD_PRELOAD first)
 *   libidk-gl.so       — actual GL hooks + compositor (loaded by shim)
 *   libidk-overlay.so  — Vulkan hooks + main init (loaded by constructor)
 *
 * Usage:
 *   LD_PRELOAD=libidk-shim.so IDK_GL=1 IDK_SOCKET=/tmp/idk-overlay <game>
 *
 * The shim auto-loads libidk-gl.so from the same directory when the first
 * GL call is intercepted.
 *
 * REAL FUNCTION LOADING:
 *   We find the REAL dlsym/dlopen/dlerror/dlclose by parsing ELF headers
 *   of libc.so via dl_iterate_phdr — NEVER calling intercepted dlsym() or
 *   dlopen() during initialization. This avoids infinite recursion.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <fnmatch.h>

#ifndef RTLD_DEEPBIND
#define RTLD_DEEPBIND 0
#endif

#ifndef RTLD_DEFAULT
#define RTLD_DEFAULT ((void *)0)
#endif

/* ── ELF symbol resolver (MangoHud-style) ──────────────────────────── */
/* Parse ELF dynamic section to find real function addresses.
 * This avoids calling intercepted dlsym() during init. */

#include <elf.h>

/* Object handle for ELF parsing */
typedef struct {
    const char *name;
    void *addr;       /* load base / bias */
    const Elf64_Phdr *phdr;
    size_t phnum;
    Elf64_Dyn *dynamic;
    const char *strtab;
    Elf64_Sym *symtab;
    unsigned int *hash;
    unsigned int *gnu_hash;
} eh_obj_t;

/* Callback for dl_iterate_phdr — finds matching library */
static int eh_find_cb(struct dl_phdr_info *info, size_t size, void *arg) {
    eh_obj_t *target = (eh_obj_t *)arg;
    (void)size;
    if (target->name == NULL) {
        if (strcmp(info->dlpi_name, "") != 0) return 0;
    } else if (fnmatch(target->name, info->dlpi_name, 0) != 0) {
        return 0;
    }
    target->addr = (void *)info->dlpi_addr;
    target->phdr = info->dlpi_phdr;
    target->phnum = info->dlpi_phnum;
    return 1; /* stop iteration */
}

/* Find shared object by name using dl_iterate_phdr */
static int eh_find_obj(eh_obj_t *obj, const char *soname) {
    obj->phdr = NULL;
    obj->name = soname;

    dl_iterate_phdr(eh_find_cb, obj);

    if (!obj->phdr) return -1;

    /* Find PT_DYNAMIC segment */
    for (size_t i = 0; i < obj->phnum; i++) {
        if (obj->phdr[i].p_type == PT_DYNAMIC) {
            obj->dynamic = (Elf64_Dyn *)(obj->phdr[i].p_vaddr + (uintptr_t)obj->addr);
            break;
        }
    }

    if (!obj->dynamic) return -1;

    /* Parse .dynamic to find DT_STRTAB, DT_SYMTAB, DT_HASH, DT_GNU_HASH */
    obj->strtab = NULL;
    obj->symtab = NULL;
    obj->hash = NULL;
    obj->gnu_hash = NULL;

    for (Elf64_Dyn *d = obj->dynamic; d->d_tag != DT_NULL; d++) {
        uintptr_t base = (uintptr_t)obj->addr;
        if (d->d_tag == DT_STRTAB) obj->strtab = (const char *)(base + d->d_un.d_ptr);
        else if (d->d_tag == DT_SYMTAB) obj->symtab = (Elf64_Sym *)(base + d->d_un.d_ptr);
        else if (d->d_tag == DT_HASH) obj->hash = (unsigned int *)(base + d->d_un.d_ptr);
        else if (d->d_tag == DT_GNU_HASH) obj->gnu_hash = (unsigned int *)(base + d->d_un.d_ptr);
    }

    if (!obj->strtab || !obj->symtab) return -1;
    return 0;
}

/* Look up symbol by name using ELF hash */
static int eh_find_sym(eh_obj_t *obj, const char *name, void **to) {
    if (!obj->symtab || !obj->strtab) return -1;

    /* Try GNU hash first */
    if (obj->gnu_hash) {
        unsigned int nbuckets = obj->gnu_hash[0];
        unsigned int *buckets = &obj->gnu_hash[4];
        unsigned int *chain = &buckets[nbuckets];

        /* Simple linear search through buckets (simplified GNU hash) */
        for (unsigned int i = 0; i < nbuckets; i++) {
            unsigned int bucket = buckets[i];
            if (bucket == 0) continue;

            unsigned int h = 0;
            const char *s = name;
            while (*s) {
                h = (h << 5) + h + *s++;
            }

            unsigned int idx = bucket;
            while (idx < obj->symtab - (Elf64_Sym *)0) {
                Elf64_Sym *sym = &obj->symtab[idx];
                if (sym->st_name && strcmp(&obj->strtab[sym->st_name], name) == 0) {
                    *to = (void *)((uintptr_t)obj->addr + sym->st_value);
                    return 0;
                }
                if (chain[idx] & 1) break; /* end of chain */
                idx++;
            }
        }
    }

    /* Fallback: linear search through dynsym */
    for (Elf64_Sym *sym = obj->symtab; sym->st_name; sym++) {
        const char *sym_name = &obj->strtab[sym->st_name];
        /* Match ignoring GLIBC version suffix (e.g. "dlsym@@GLIBC_2.34") */
        const char *ver = strchr(sym_name, '@');
        size_t len = ver ? (ver - sym_name) : strlen(sym_name);
        if (strncmp(sym_name, name, len) == 0 && strlen(name) == len &&
            ELF64_ST_BIND(sym->st_info) != STB_LOCAL) {
            *to = (void *)((uintptr_t)obj->addr + sym->st_value);
            return 0;
        }
    }

    return -1;
}

/* ── Real function pointers (populated by load_real_functions) ──────── */
static void *(*real_dlsym)(void *, const char *) = NULL;
static const char *(*real_dlerror)(void) = NULL;
static void *(*real_dlopen)(const char *, int) = NULL;
static int (*real_dlclose)(void *) = NULL;

static void load_real_functions(void) {
    static int done = 0;
    if (done) return;

    eh_obj_t obj;
    memset(&obj, 0, sizeof(obj));

    /* Try libraries in order (MangoHud-style) */
    const char *libs[] = {
        "*libc.so*",
        "*libdl.so*",
    };

    for (size_t i = 0; i < sizeof(libs) / sizeof(*libs); i++) {
        int ret = eh_find_obj(&obj, libs[i]);
        if (ret != 0) continue;

        void *dlopen_ptr = NULL, *dlsym_ptr = NULL, *dlerror_ptr = NULL;
        eh_find_sym(&obj, "dlopen", &dlopen_ptr);
        eh_find_sym(&obj, "dlsym", &dlsym_ptr);
        eh_find_sym(&obj, "dlerror", &dlerror_ptr);

        if (dlopen_ptr && dlsym_ptr && dlerror_ptr) {
            real_dlopen = (typeof(real_dlopen))dlopen_ptr;
            real_dlsym = (typeof(real_dlsym))dlsym_ptr;
            real_dlerror = (typeof(real_dlerror))dlerror_ptr;

            /* Also find dlclose */
            eh_find_sym(&obj, "dlclose", (void **)&real_dlclose);

            done = 1;
            fprintf(stderr, "[idk-shim] Loaded real functions from %s: dlsym=%p dlopen=%p\n",
                    libs[i], (void *)real_dlsym, (void *)real_dlopen);
            return;
        }
    }

    fprintf(stderr, "[idk-shim] WARNING: Failed to find real dlsym/dlopen\n");
    done = 1; /* prevent retrying */
}

/* ── State ────────────────────────────────────────────────────────────── */

static bool g_loaded = false;
static void *g_gl_handle = NULL;

/* ── GL hook function types ──────────────────────────────────────────── */

typedef void *(*PFN_eglGetProcAddress)(const char *);
typedef unsigned int (*PFN_eglSwapBuffers)(void *dpy, void *surface);
typedef void *(*PFN_eglGetDisplay)(void *native_display);
typedef void *(*PFN_eglGetPlatformDisplay)(unsigned int platform,
                                            void *native_display,
                                            const intptr_t *attrib_list);
typedef int (*PFN_eglTerminate)(void *display);
typedef unsigned int (*PFN_eglDestroyContext)(void *dpy, void *ctx);

/* Hook function pointers (resolved from libidk-gl.so) */
static PFN_eglGetProcAddress fn_eglGetProcAddress = NULL;
static PFN_eglSwapBuffers fn_eglSwapBuffers = NULL;
static PFN_eglGetDisplay fn_eglGetDisplay = NULL;
static PFN_eglGetPlatformDisplay fn_eglGetPlatformDisplay = NULL;
static PFN_eglTerminate fn_eglTerminate = NULL;
static PFN_eglDestroyContext fn_eglDestroyContext = NULL;

/* ── Load libidk-gl.so ───────────────────────────────────────────────── */

static bool load_gl_hook(void) {
    if (g_loaded) return true;

    /* Find our own path using dladdr */
    Dl_info info;
    memset(&info, 0, sizeof(info));
    if (!dladdr((void *)load_gl_hook, &info) || !info.dli_fname) {
        fprintf(stderr, "[idk-shim] dladdr failed\n");
        return false;
    }

    /* Extract directory */
    const char *slash = strrchr(info.dli_fname, '/');
    char dir[1024];
    if (slash) {
        size_t len = slash - info.dli_fname;
        snprintf(dir, sizeof(dir), "%.*s", (int)len, info.dli_fname);
    } else {
        strcpy(dir, ".");
    }

    /* Build path to libidk-gl.so */
    char lib_path[1024];
    snprintf(lib_path, sizeof(lib_path), "%s/libidk-gl.so", dir);

    /* Check if environment overrides the path */
    const char *env_lib = getenv("IDK_GL_LIB");
    if (env_lib) {
        snprintf(lib_path, sizeof(lib_path), "%s", env_lib);
    }

    fprintf(stderr, "[idk-shim] Loading GL hook: %s\n", lib_path);

    g_gl_handle = real_dlopen(lib_path, RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
    if (!g_gl_handle) {
        fprintf(stderr, "[idk-shim] Failed to load libidk-gl.so: %s\n",
                real_dlerror ? real_dlerror() : "unknown error");
        return false;
    }

    /* Resolve hook functions from libidk-gl.so */
    if (real_dlsym) {
        fn_eglGetProcAddress = (PFN_eglGetProcAddress)real_dlsym(g_gl_handle, "idk_eglGetProcAddress");
        fn_eglSwapBuffers = (PFN_eglSwapBuffers)real_dlsym(g_gl_handle, "idk_eglSwapBuffers");
        fn_eglGetDisplay = (PFN_eglGetDisplay)real_dlsym(g_gl_handle, "idk_eglGetDisplay");
        fn_eglGetPlatformDisplay = (PFN_eglGetPlatformDisplay)real_dlsym(g_gl_handle, "idk_eglGetPlatformDisplay");
        fn_eglTerminate = (PFN_eglTerminate)real_dlsym(g_gl_handle, "idk_eglTerminate");
        fn_eglDestroyContext = (PFN_eglDestroyContext)real_dlsym(g_gl_handle, "idk_eglDestroyContext");
    }

    g_loaded = true;
    fprintf(stderr, "[idk-shim] GL hooks loaded (eglSwapBuffers=%p)\n", (void *)fn_eglSwapBuffers);
    return true;
}

/* ── Hook forwarding functions ───────────────────────────────────────── */

static void *hook_eglGetProcAddress(const char *procName) {
    if (!g_loaded) load_gl_hook();

    /* If we have our own hook, return it */
    if (g_loaded && fn_eglGetProcAddress) {
        void *hook = fn_eglGetProcAddress(procName);
        if (hook) return hook;
    }

    /* Fall back to real eglGetProcAddress */
    if (real_dlsym) {
        void *lib_egl = real_dlopen("libEGL.so.1", RTLD_LAZY);
        if (lib_egl) {
            PFN_eglGetProcAddress real_fn =
                (PFN_eglGetProcAddress)real_dlsym(lib_egl, "eglGetProcAddress");
            if (real_fn) return real_fn(procName);
            real_dlclose(lib_egl);
        }
    }
    return NULL;
}

static unsigned int hook_eglSwapBuffers(void *dpy, void *surface) {
    if (!g_loaded) load_gl_hook();

    if (!g_loaded || !fn_eglSwapBuffers) {
        /* Fallback: call real eglSwapBuffers */
        void *lib_egl = real_dlopen("libEGL.so.1", RTLD_LAZY);
        if (lib_egl) {
            PFN_eglSwapBuffers real_fn =
                (PFN_eglSwapBuffers)real_dlsym(lib_egl, "eglSwapBuffers");
            if (real_fn) {
                unsigned int res = real_fn(dpy, surface);
                real_dlclose(lib_egl);
                return res;
            }
            real_dlclose(lib_egl);
        }
        return 0;
    }

    return fn_eglSwapBuffers(dpy, surface);
}

static void *hook_eglGetDisplay(void *native_display) {
    if (!g_loaded) load_gl_hook();

    if (g_loaded && fn_eglGetDisplay) {
        return fn_eglGetDisplay(native_display);
    }

    /* Fallback */
    void *lib_egl = real_dlopen("libEGL.so.1", RTLD_LAZY);
    if (lib_egl) {
        PFN_eglGetDisplay real_fn =
            (PFN_eglGetDisplay)real_dlsym(lib_egl, "eglGetDisplay");
        if (real_fn) {
            void *res = real_fn(native_display);
            real_dlclose(lib_egl);
            return res;
        }
        real_dlclose(lib_egl);
    }
    return NULL;
}

static void *hook_eglGetPlatformDisplay(unsigned int platform,
                                         void *native_display,
                                         const intptr_t *attrib_list) {
    if (!g_loaded) load_gl_hook();

    if (g_loaded && fn_eglGetPlatformDisplay) {
        return fn_eglGetPlatformDisplay(platform, native_display, attrib_list);
    }

    /* Fallback */
    void *lib_egl = real_dlopen("libEGL.so.1", RTLD_LAZY);
    if (lib_egl) {
        PFN_eglGetPlatformDisplay real_fn =
            (PFN_eglGetPlatformDisplay)real_dlsym(lib_egl, "eglGetPlatformDisplay");
        if (real_fn) {
            void *res = real_fn(platform, native_display, attrib_list);
            real_dlclose(lib_egl);
            return res;
        }
        real_dlclose(lib_egl);
    }
    return NULL;
}

static int hook_eglTerminate(void *display) {
    if (!g_loaded) load_gl_hook();

    if (g_loaded && fn_eglTerminate) {
        return fn_eglTerminate(display);
    }

    /* Fallback */
    void *lib_egl = real_dlopen("libEGL.so.1", RTLD_LAZY);
    if (lib_egl) {
        PFN_eglTerminate real_fn =
            (PFN_eglTerminate)real_dlsym(lib_egl, "eglTerminate");
        if (real_fn) {
            int res = real_fn(display);
            real_dlclose(lib_egl);
            return res;
        }
        real_dlclose(lib_egl);
    }
    return 0;
}

static unsigned int hook_eglDestroyContext(void *dpy, void *ctx) {
    if (!g_loaded) load_gl_hook();

    if (g_loaded && fn_eglDestroyContext) {
        return fn_eglDestroyContext(dpy, ctx);
    }

    /* Fallback */
    void *lib_egl = real_dlopen("libEGL.so.1", RTLD_LAZY);
    if (lib_egl) {
        PFN_eglDestroyContext real_fn =
            (PFN_eglDestroyContext)real_dlsym(lib_egl, "eglDestroyContext");
        if (real_fn) {
            unsigned int res = real_fn(dpy, ctx);
            real_dlclose(lib_egl);
            return res;
        }
        real_dlclose(lib_egl);
    }
    return 0;
}

/* ── Hook table ──────────────────────────────────────────────────────── */

struct hook_entry {
    const char *name;
    void *ptr;
};

static const struct hook_entry hooks[] = {
    {"eglGetProcAddress",  (void *)hook_eglGetProcAddress},
    {"eglSwapBuffers",     (void *)hook_eglSwapBuffers},
    {"eglGetDisplay",      (void *)hook_eglGetDisplay},
    {"eglGetPlatformDisplay", (void *)hook_eglGetPlatformDisplay},
    {"eglTerminate",       (void *)hook_eglTerminate},
    {"eglDestroyContext",  (void *)hook_eglDestroyContext},
};

#define NUM_HOOKS (sizeof(hooks) / sizeof(hooks[0]))

/* ── dlsym() interceptor ─────────────────────────────────────────────── */

void *dlsym(void *handle, const char *name) {
    /* Load real functions if not yet loaded */
    if (!real_dlsym) load_real_functions();

    /* If still not loaded (e.g., recursion during init), return NULL */
    if (!real_dlsym) return NULL;

    /* Intercept known GL/EGL hooks */
    for (size_t i = 0; i < NUM_HOOKS; i++) {
        if (strcmp(hooks[i].name, name) == 0) {
            return hooks[i].ptr;
        }
    }

    /* For all other symbols, use real dlsym */
    return real_dlsym(handle, name);
}

/* ── Constructor (optional, for Vulkan path) ─────────────────────────── */

__attribute__((constructor))
static void shim_init(void) {
    load_real_functions();
}
