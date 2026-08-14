/*
 * shm_layout.h - Shared-memory transport wire layout (single protocol spec)
 *
 * 4096-byte page shared between consumer (overlay/compositor) and producer
 * (webview). Offsets are fixed ABI - do not change.
 *
 *  offset  size  field
 *  ------  ----  --------------------------------------------------
 *  0       4     MAGIC        SHM_MAGIC_VAL (0x4D485349)
 *  4       4     PROD_STATE   atomic: 0=idle, 1=ready, 2=accepted, -1=dead
 *  8       4     CONS_STATE   atomic: 1=waiting, 2=ready, -1=dead
 *  12      4     PROD_PID     producer pid (0 = none)
 *  16      4     CONS_PID     consumer pid
 *  20      4     DMABUF_NFD   fd count (0..4)
 *  24      16    DMABUF_FDS   producer-side fd numbers [4]
 *  40      28    HDR          idk_frame_header_t / idk_input_event_t
 *  68      16    ACK          idk_ack_msg_t (trailing pad bytes 80-83 alias
 *                             SLOT_STATE; the SLOT_STATE store wins)
 *  80      4     SLOT_STATE   SLOT_EMPTY / SLOT_FRAME / SLOT_ACK /
 *                             SLOT_CONSUMED
 *  84      4     FRAME_SEQ    monotonically increasing frame counter
 *  88      4     REQ_SEQ      request counter (consumer -> producer)
 *  92      4     EVENTFD      eventfd number for input notify (0 = none)
 *  96      4000  reserved
 */
#ifndef IDK_SHM_LAYOUT_H
#define IDK_SHM_LAYOUT_H

#define SHM_SIZE 4096

#define SHM_O_MAGIC 0
#define SHM_O_PROD_STATE 4
#define SHM_O_CONS_STATE 8
#define SHM_O_PROD_PID 12
#define SHM_O_CONS_PID 16
#define SHM_O_DMABUF_NFD 20
#define SHM_O_DMABUF_FDS 24
#define SHM_O_HDR 40
#define SHM_O_ACK 68
#define SHM_O_SLOT_STATE 80
#define SHM_O_FRAME_SEQ 84
#define SHM_O_REQ_SEQ 88
#define SHM_O_EVENTFD 92

_Static_assert(SHM_O_EVENTFD + 4 <= SHM_SIZE, "SHM layout exceeds page size");

#define SHM_MAGIC_VAL 0x4D485349

#define SLOT_EMPTY 0
#define SLOT_FRAME 1
#define SLOT_ACK 2
#define SLOT_CONSUMED 3

#endif /* IDK_SHM_LAYOUT_H */
