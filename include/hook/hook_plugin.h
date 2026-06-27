#ifndef IDK_HOOK_PLUGIN_H
#define IDK_HOOK_PLUGIN_H

/* Plugin descriptor for hook modules.
 *
 * Each graphics API backend (EGL, GLX, Vulkan syringe) registers one of
 * these so overlay.c can discover and initialize it generically - no
 * hardcoded per-API code needed in the orchestrator.
 *
 * Probing is done generically: overlay.c calls dlopen(name, NOLOAD) for
 * each string in lib_patterns[]. The first match triggers init().
 */
typedef struct idk_hook_plugin {
    const char *name;          /* short tag: "egl", "glx", "vk-syringe" */
    const char *lib_patterns[4]; /* null-terminated list of .so names to probe */

    /* Install hooks. Returns 0 on success, -1 on failure.
     * May be called multiple times (retried from polling loop). */
    int  (*init)(void);

    /* Remove hooks. Called during shutdown. */
    void (*shutdown)(void);
} idk_hook_plugin_t;

/* Built-in plugins - declared here, defined in their respective hook files */
extern idk_hook_plugin_t idk_plugin_egl;
extern idk_hook_plugin_t idk_plugin_glx;

#endif /* IDK_HOOK_PLUGIN_H */
