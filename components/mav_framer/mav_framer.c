#include "mav_framer.h"

#include <string.h>

/* ---- boundary scanner ---------------------------------------------------- */

void mav_scan_reset(mav_scan_t *s)
{
    memset(s, 0, sizeof(*s));
}

bool mav_scan_at_boundary(const mav_scan_t *s)
{
    return s->got == 0u;
}

mav_scan_res_t mav_scan_byte(mav_scan_t *s, uint8_t b)
{
    if (s->got == 0u) {
        if (b != MAV_STX_V1 && b != MAV_STX_V2) {
            return MAV_SCAN_RAW;
        }
        s->hdr[0] = b;
        s->got    = 1u;
        s->need   = 0u;
        return MAV_SCAN_IN_PKT;
    }

    if (s->got < sizeof(s->hdr)) {
        s->hdr[s->got] = b;
    }
    s->got++;

    if (s->need == 0u) {
        if (s->hdr[0] == MAV_STX_V1 && s->got >= 2u) {
            /* STX len seq sys comp msgid | payload | crc16 */
            s->need = (uint16_t)(8u + s->hdr[1]);
        } else if (s->hdr[0] == MAV_STX_V2 && s->got >= 3u) {
            /* STX len incompat compat seq sys comp msgid[3] | payload | crc16 | [sig13] */
            s->need = (uint16_t)(12u + s->hdr[1] + ((s->hdr[2] & 0x01u) ? 13u : 0u));
        }
    }

    if (s->need != 0u && s->got >= s->need) {
        s->got  = 0u;
        s->need = 0u;
        return MAV_SCAN_PKT_END;
    }
    return MAV_SCAN_IN_PKT;
}

/* ---- framer -------------------------------------------------------------- */

void mav_framer_init(mav_framer_t *f, uint16_t out_cap)
{
    memset(f, 0, sizeof(*f));
    if (out_cap < MAV_MAX_PACKET) {
        out_cap = MAV_MAX_PACKET;
    }
    if (out_cap > MAV_FRAMER_MAX_OUT) {
        out_cap = MAV_FRAMER_MAX_OUT;
    }
    f->out_cap = out_cap;
}

static void emit_out(mav_framer_t *f, mav_emit_fn emit, void *ctx)
{
    if (f->out_len == 0u) {
        return;
    }
    emit(ctx, f->out, f->out_len);
    f->frames++;
    f->out_len = 0u;
}

/* Append bytes to `out`, emitting first if they would not fit. n <= out_cap. */
static void append_out(mav_framer_t *f, const uint8_t *src, uint16_t n,
                       uint32_t now_ms, mav_emit_fn emit, void *ctx)
{
    if ((uint32_t)f->out_len + n > f->out_cap) {
        emit_out(f, emit, ctx);
    }
    if (f->out_len == 0u) {
        f->out_since_ms = now_ms;
    }
    memcpy(&f->out[f->out_len], src, n);
    f->out_len = (uint16_t)(f->out_len + n);
}

void mav_framer_push(mav_framer_t *f, const uint8_t *data, size_t len,
                     uint32_t now_ms, mav_emit_fn emit, void *ctx)
{
    for (size_t i = 0; i < len; i++) {
        const uint8_t b = data[i];
        switch (mav_scan_byte(&f->scan, b)) {
        case MAV_SCAN_RAW:
            append_out(f, &b, 1u, now_ms, emit, ctx);
            f->raw_bytes++;
            break;
        case MAV_SCAN_IN_PKT:
            if (f->pkt_len < MAV_MAX_PACKET) {
                f->pkt[f->pkt_len++] = b;
            }
            break;
        case MAV_SCAN_PKT_END:
            if (f->pkt_len < MAV_MAX_PACKET) {
                f->pkt[f->pkt_len++] = b;
            }
            f->last_version = (f->pkt[0] == MAV_STX_V2) ? 2u : 1u;
            append_out(f, f->pkt, f->pkt_len, now_ms, emit, ctx);
            f->pkt_len = 0u;
            f->packets++;
            if (f->out_len == f->out_cap) {
                emit_out(f, emit, ctx);
            }
            break;
        default:
            break;
        }
    }
}

void mav_framer_poll(mav_framer_t *f, uint32_t now_ms, uint32_t max_hold_ms,
                     mav_emit_fn emit, void *ctx)
{
    if (f->out_len > 0u && (uint32_t)(now_ms - f->out_since_ms) >= max_hold_ms) {
        emit_out(f, emit, ctx);
    }
}

void mav_framer_flush(mav_framer_t *f, mav_emit_fn emit, void *ctx)
{
    if (f->pkt_len > 0u) {
        /* Unfinished packet: forward its bytes anyway (transparency). */
        append_out(f, f->pkt, f->pkt_len, f->out_since_ms, emit, ctx);
        f->raw_bytes += f->pkt_len;
        f->pkt_len = 0u;
        mav_scan_reset(&f->scan);
    }
    emit_out(f, emit, ctx);
}

void mav_framer_discard(mav_framer_t *f)
{
    f->pkt_len = 0u;
    f->out_len = 0u;
    mav_scan_reset(&f->scan);
}
