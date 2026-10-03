/*
 * link_proto - over-the-air frame format, sequence tracking and link state
 * machine. Pure C99, no ESP-IDF dependencies (host unit-testable).
 *
 * Frame layout (all multi-byte fields little-endian):
 *   [0]      magic 0xE5
 *   [1]      version (high nibble) | type (low nibble)
 *   [2..3]   network id
 *   [4..5]   sequence number
 *   [6..7]   payload length
 *   [8..]    payload
 *   [+2]     CRC-16/CCITT-FALSE over bytes [0 .. 8+len)
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LINK_MAGIC        0xE5u
#define LINK_VERSION      1u
#define LINK_HDR_LEN      8u
#define LINK_CRC_LEN      2u
#define LINK_MAX_PAYLOAD  290u /* >= largest MAVLink v2 packet (280) */
#define LINK_MAX_FRAME    (LINK_HDR_LEN + LINK_MAX_PAYLOAD + LINK_CRC_LEN)

typedef enum {
    LINK_T_DATA      = 1, /* opaque serial bytes */
    LINK_T_HEARTBEAT = 2, /* link keep-alive + link quality */
    LINK_T_BEACON    = 3, /* pairing beacon (broadcast, HMAC-authenticated) */
} link_type_t;

typedef enum {
    LINK_OK = 0,
    LINK_E_SHORT,   /* shorter than header + CRC */
    LINK_E_MAGIC,
    LINK_E_VERSION,
    LINK_E_TYPE,
    LINK_E_LEN,     /* length field disagrees with received size */
    LINK_E_CRC,
    LINK_E_NET,     /* frame belongs to a different network id */
} link_err_t;

typedef struct {
    link_type_t    type;
    uint16_t       net_id;
    uint16_t       seq;
    uint16_t       len;
    const uint8_t *payload; /* points into the decoded buffer */
} link_frame_t;

uint16_t link_crc16(const uint8_t *data, size_t len, uint16_t crc);

/* Returns total encoded length, or 0 if the arguments are invalid or the
 * frame would not fit in `cap`. */
size_t link_frame_encode(uint8_t *out, size_t cap, link_type_t type,
                         uint16_t net_id, uint16_t seq,
                         const uint8_t *payload, uint16_t len);

link_err_t link_frame_decode(const uint8_t *in, size_t len,
                             uint16_t expect_net_id, link_frame_t *out);

/* ---- heartbeat payload --------------------------------------------------- */

#define LINK_HB_LEN 8u

typedef struct {
    uint8_t  state;      /* sender's link_state_t */
    int8_t   rssi_dbm;   /* RSSI of the last frame the sender received from us */
    int8_t   noise_dbm;  /* sender's noise floor */
    uint8_t  txbuf_pct;  /* sender's free air-TX buffer, percent */
    uint16_t rx_lost;    /* frames the sender detected as lost (saturating) */
    uint16_t rx_errors;  /* frames the sender rejected (saturating) */
} link_hb_t;

size_t link_hb_encode(const link_hb_t *hb, uint8_t *out, size_t cap);
bool   link_hb_decode(const uint8_t *in, size_t len, link_hb_t *hb);

/* ---- sequence tracking --------------------------------------------------- */

/* Frames are accepted only if they advance the sequence by 1..GAP-1. Anything
 * else (duplicates, reordering, replays, wild jumps) is rejected. A new pairing
 * session starts a fresh window (link_seq_reset), so no in-session resync is
 * needed; the link-lost timeout bounds how far a legitimate gap can grow. */
#define LINK_SEQ_WINDOW 1024

typedef struct {
    bool     valid;
    uint16_t last;
    uint32_t lost;    /* frames skipped over */
    uint32_t dup;     /* rejected: duplicate, stale or out of window */
} link_seq_t;

typedef enum { LINK_SEQ_ACCEPT = 0, LINK_SEQ_REJECT } link_seq_res_t;

void           link_seq_reset(link_seq_t *s);
link_seq_res_t link_seq_check(link_seq_t *s, uint16_t seq);

/* ---- link state machine -------------------------------------------------- */

typedef enum {
    LINK_SEARCHING = 0, /* no authenticated peer traffic */
    LINK_CONNECTED,
    LINK_DEGRADED,      /* peer silent longer than the degraded threshold */
} link_state_t;

typedef enum {
    LINK_EV_NONE = 0,
    LINK_EV_CONNECTED,
    LINK_EV_DEGRADED,
    LINK_EV_RECOVERED,
    LINK_EV_LOST,
} link_event_t;

typedef struct {
    link_state_t state;
    uint32_t     last_rx_ms;
    uint32_t     degraded_ms;
    uint32_t     lost_ms;
    uint32_t     losses;
} link_sm_t;

void         link_sm_init(link_sm_t *sm, uint32_t degraded_ms, uint32_t lost_ms);
link_event_t link_sm_on_peer_rx(link_sm_t *sm, uint32_t now_ms);
link_event_t link_sm_tick(link_sm_t *sm, uint32_t now_ms);
link_event_t link_sm_force_lost(link_sm_t *sm);
bool         link_sm_is_up(const link_sm_t *sm);
const char  *link_state_name(link_state_t s);

#ifdef __cplusplus
}
#endif
