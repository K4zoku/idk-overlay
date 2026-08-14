#ifndef IDK_BROKER_H
#define IDK_BROKER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Shared broker/wine logic used by both the LD_PRELOAD overlay
 * (libidk-overlay.so) and the Vulkan layer (libidk-vklayer.so). */

extern int g_wine_detected; /* -1 unknown, 0 native, 1 wine */
extern int g_use_broker;    /* 1 = broker connected (webview spawned by broker) */
extern int g_broker_fd;     /* control socket fd kept open for EOF detection */

/* Cached wine detection: .exe name or wine libs in /proc/self/maps. */
bool detect_wine(void);
int idk_is_wine(void);

/* Connect to the host-namespace broker, send handshake describing the
 * transport/input sockets, wait for ack. Sets IDK_TP_ABSTRACT /
 * IDK_INPUT_ABSTRACT env for the compositor. Returns 0 on success. */
int connect_via_broker(void);

#ifdef __cplusplus
}
#endif

#endif /* IDK_BROKER_H */
