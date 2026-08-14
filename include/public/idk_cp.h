/*
 * idk_cp.h - Broker control-plane wire types
 *
 * Broker control-plane handshake: sent by the overlay (injected .so,
 * Wine mount namespace) immediately after connecting to the broker's
 * abstract socket "\0idk_broker_<uid>". The broker uses the info to
 * exec idk-webview with the right env so the webview can connect
 * directly to the overlay's transport/input sockets — no broker on the
 * hot path.
 *
 * Socket names are stored WITHOUT a leading NUL. The sender (overlay)
 * is responsible for prepending "\0" when binding; the consumer reads
 * the names here and prepends "\0" at connect time.
 *
 * ACK byte codes (reply to the handshake): 0 = accepted,
 * 1 = rejected (DMABUF not supported).
 */
#ifndef IDK_CP_H
#define IDK_CP_H

#include <assert.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IDK_CP_ID_OVERLAY 1u

#pragma pack(push, 1)
typedef struct idk_cp_handshake {
  uint32_t identity;     /* offset  0 - must be IDK_CP_ID_OVERLAY          */
  char comm[64];         /* offset  4 - game process name (NUL-term)       */
  char tp_socket[64];    /* offset 68 - abstract name, no leading '\0'      */
  char input_socket[64]; /* offset 132 - abstract name, no leading '\0'     */
  uint8_t tp_backend;    /* offset 196 - 0=socket, 1=shm                   */
  uint8_t no_dmabuf;     /* offset 197 - 1=compositor can't import dmabuf  */
  uint8_t _pad[6];       /* offset 198 - reserved                           */
} idk_cp_handshake_t;    /* total 204 bytes                                  */
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(idk_cp_handshake_t) == 204, "idk_cp_handshake_t must be 204 bytes");
#else
_Static_assert(sizeof(idk_cp_handshake_t) == 204, "idk_cp_handshake_t must be 204 bytes");
#endif

#ifdef __cplusplus
}
#endif

#endif /* IDK_CP_H */
