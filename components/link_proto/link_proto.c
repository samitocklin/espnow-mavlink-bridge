#include "link_proto.h"

#include <string.h>

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

uint16_t link_crc16(const uint8_t *data, size_t len, uint16_t crc)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static bool type_valid(unsigned t)
{
    return t == LINK_T_DATA || t == LINK_T_HEARTBEAT || t == LINK_T_BEACON;
}

size_t link_frame_encode(uint8_t *out, size_t cap, link_type_t type,
                         uint16_t net_id, uint16_t seq,
                         const uint8_t *payload, uint16_t len)
{
    const size_t total = LINK_HDR_LEN + (size_t)len + LINK_CRC_LEN;
    if (out == NULL || !type_valid((unsigned)type) || len > LINK_MAX_PAYLOAD ||
        (len > 0u && payload == NULL) || total > cap) {
        return 0;
    }
    out[0] = LINK_MAGIC;
    out[1] = (uint8_t)((LINK_VERSION << 4) | ((unsigned)type & 0x0Fu));
    put_u16(&out[2], net_id);
    put_u16(&out[4], seq);
    put_u16(&out[6], len);
    if (len > 0u) {
        memcpy(&out[LINK_HDR_LEN], payload, len);
    }
    put_u16(&out[LINK_HDR_LEN + len], link_crc16(out, LINK_HDR_LEN + (size_t)len, 0xFFFFu));
    return total;
}

link_err_t link_frame_decode(const uint8_t *in, size_t len,
                             uint16_t expect_net_id, link_frame_t *out)
{
    if (in == NULL || out == NULL || len < LINK_HDR_LEN + LINK_CRC_LEN) {
        return LINK_E_SHORT;
    }
    if (in[0] != LINK_MAGIC) {
        return LINK_E_MAGIC;
    }
    if ((in[1] >> 4) != LINK_VERSION) {
        return LINK_E_VERSION;
    }
    const unsigned type = in[1] & 0x0Fu;
    if (!type_valid(type)) {
        return LINK_E_TYPE;
    }
    const uint16_t plen = get_u16(&in[6]);
    if (plen > LINK_MAX_PAYLOAD || (size_t)plen + LINK_HDR_LEN + LINK_CRC_LEN != len) {
        return LINK_E_LEN;
    }
    if (link_crc16(in, LINK_HDR_LEN + (size_t)plen, 0xFFFFu) != get_u16(&in[LINK_HDR_LEN + plen])) {
        return LINK_E_CRC;
    }
    const uint16_t net = get_u16(&in[2]);
    if (net != expect_net_id) {
        return LINK_E_NET;
    }
    out->type    = (link_type_t)type;
    out->net_id  = net;
    out->seq     = get_u16(&in[4]);
    out->len     = plen;
    out->payload = &in[LINK_HDR_LEN];
    return LINK_OK;
}

/* ---- heartbeat ----------------------------------------------------------- */

size_t link_hb_encode(const link_hb_t *hb, uint8_t *out, size_t cap)
{
    if (hb == NULL || out == NULL || cap < LINK_HB_LEN) {
        return 0;
    }
    out[0] = hb->state;
    out[1] = (uint8_t)hb->rssi_dbm;
    out[2] = (uint8_t)hb->noise_dbm;
    out[3] = hb->txbuf_pct;
    put_u16(&out[4], hb->rx_lost);
    put_u16(&out[6], hb->rx_errors);
    return LINK_HB_LEN;
}

bool link_hb_decode(const uint8_t *in, size_t len, link_hb_t *hb)
{
    if (in == NULL || hb == NULL || len != LINK_HB_LEN) {
        return false;
    }
    hb->state     = in[0];
    hb->rssi_dbm  = (int8_t)in[1];
    hb->noise_dbm = (int8_t)in[2];
    hb->txbuf_pct = in[3] > 100u ? 100u : in[3];
    hb->rx_lost   = get_u16(&in[4]);
    hb->rx_errors = get_u16(&in[6]);
    return true;
}

/* ---- sequence tracking --------------------------------------------------- */

void link_seq_reset(link_seq_t *s)
{
    memset(s, 0, sizeof(*s));
}

link_seq_res_t link_seq_check(link_seq_t *s, uint16_t seq)
{
    if (!s->valid) {
        s->valid = true;
        s->last  = seq;
        return LINK_SEQ_ACCEPT;
    }
    const uint16_t diff = (uint16_t)(seq - s->last);
    if (diff > 0u && diff < LINK_SEQ_WINDOW) {
        s->lost += (uint32_t)(diff - 1u);
        s->last = seq;
        return LINK_SEQ_ACCEPT;
    }
    s->dup++;
    return LINK_SEQ_REJECT;
}

/* ---- state machine ------------------------------------------------------- */

void link_sm_init(link_sm_t *sm, uint32_t degraded_ms, uint32_t lost_ms)
{
    memset(sm, 0, sizeof(*sm));
    sm->state       = LINK_SEARCHING;
    sm->degraded_ms = degraded_ms;
    sm->lost_ms     = lost_ms > degraded_ms ? lost_ms : degraded_ms + 1u;
}

link_event_t link_sm_on_peer_rx(link_sm_t *sm, uint32_t now_ms)
{
    sm->last_rx_ms = now_ms;
    switch (sm->state) {
    case LINK_SEARCHING:
        sm->state = LINK_CONNECTED;
        return LINK_EV_CONNECTED;
    case LINK_DEGRADED:
        sm->state = LINK_CONNECTED;
        return LINK_EV_RECOVERED;
    case LINK_CONNECTED:
    default:
        return LINK_EV_NONE;
    }
}

link_event_t link_sm_tick(link_sm_t *sm, uint32_t now_ms)
{
    if (sm->state == LINK_SEARCHING) {
        return LINK_EV_NONE;
    }
    const uint32_t age = now_ms - sm->last_rx_ms; /* wrap-safe */
    if (age >= sm->lost_ms) {
        return link_sm_force_lost(sm);
    }
    if (age >= sm->degraded_ms && sm->state == LINK_CONNECTED) {
        sm->state = LINK_DEGRADED;
        return LINK_EV_DEGRADED;
    }
    return LINK_EV_NONE;
}

link_event_t link_sm_force_lost(link_sm_t *sm)
{
    if (sm->state == LINK_SEARCHING) {
        return LINK_EV_NONE;
    }
    sm->state = LINK_SEARCHING;
    sm->losses++;
    return LINK_EV_LOST;
}

bool link_sm_is_up(const link_sm_t *sm)
{
    return sm->state != LINK_SEARCHING;
}

const char *link_state_name(link_state_t s)
{
    switch (s) {
    case LINK_SEARCHING: return "SEARCHING";
    case LINK_CONNECTED: return "CONNECTED";
    case LINK_DEGRADED:  return "DEGRADED";
    default:             return "?";
    }
}
