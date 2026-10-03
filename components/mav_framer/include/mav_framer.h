/*
 * mav_framer - MAVLink packet-boundary detection and air-frame packing.
 * Pure C99, no ESP-IDF dependencies (host unit-testable).
 *
 * The bridge is byte-transparent: every input byte is forwarded, in order,
 * whether or not it belongs to a MAVLink packet. Boundary detection is only
 * used to (a) avoid splitting a packet across two radio frames, so one lost
 * frame costs whole packets instead of corrupting neighbours, and (b) find
 * safe points to inject RADIO_STATUS into the outgoing serial stream.
 * CRCs are deliberately not checked here; endpoints do that.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAV_STX_V1     0xFEu
#define MAV_STX_V2     0xFDu
#define MAV_MAX_PACKET 280u /* v2: 10 hdr + 255 payload + 2 crc + 13 signature */

/* ---- boundary scanner ---------------------------------------------------- */

typedef enum {
    MAV_SCAN_RAW = 0, /* byte is outside any packet */
    MAV_SCAN_IN_PKT,  /* byte belongs to an unfinished packet */
    MAV_SCAN_PKT_END, /* byte completed a packet */
} mav_scan_res_t;

typedef struct {
    uint16_t got;    /* bytes of the current packet seen so far, 0 = idle */
    uint16_t need;   /* total packet length once known, else 0 */
    uint8_t  hdr[3]; /* STX, len, incompat_flags */
} mav_scan_t;

void           mav_scan_reset(mav_scan_t *s);
mav_scan_res_t mav_scan_byte(mav_scan_t *s, uint8_t b);
bool           mav_scan_at_boundary(const mav_scan_t *s);

/* ---- framer -------------------------------------------------------------- */

#define MAV_FRAMER_MAX_OUT 290u

typedef void (*mav_emit_fn)(void *ctx, const uint8_t *data, size_t len);

typedef struct {
    mav_scan_t scan;
    uint8_t    pkt[MAV_MAX_PACKET];
    uint16_t   pkt_len;
    uint8_t    out[MAV_FRAMER_MAX_OUT];
    uint16_t   out_len;
    uint16_t   out_cap;
    uint32_t   out_since_ms;  /* when `out` last went from empty to non-empty */
    uint8_t    last_version;  /* 1 or 2 for the last complete packet, 0 unknown */
    uint32_t   packets;
    uint32_t   raw_bytes;
    uint32_t   frames;
} mav_framer_t;

/* out_cap is clamped to [MAV_MAX_PACKET, MAV_FRAMER_MAX_OUT]. */
void mav_framer_init(mav_framer_t *f, uint16_t out_cap);

/* Feed serial input. Emits a frame whenever the next packet would not fit. */
void mav_framer_push(mav_framer_t *f, const uint8_t *data, size_t len,
                     uint32_t now_ms, mav_emit_fn emit, void *ctx);

/* Emit buffered complete packets if they have waited at least max_hold_ms.
 * An unfinished packet is kept. */
void mav_framer_poll(mav_framer_t *f, uint32_t now_ms, uint32_t max_hold_ms,
                     mav_emit_fn emit, void *ctx);

/* Line went idle: emit everything, including an unfinished packet. */
void mav_framer_flush(mav_framer_t *f, mav_emit_fn emit, void *ctx);

/* Drop everything buffered (used when the link is lost). */
void mav_framer_discard(mav_framer_t *f);

#ifdef __cplusplus
}
#endif
