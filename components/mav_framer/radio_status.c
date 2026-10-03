#include "radio_status.h"

uint16_t mav_crc_x25(const uint8_t *data, size_t len, uint16_t crc)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t tmp = (uint8_t)(data[i] ^ (uint8_t)(crc & 0xFFu));
        tmp = (uint8_t)(tmp ^ (uint8_t)(tmp << 4));
        crc = (uint16_t)((crc >> 8) ^ ((uint16_t)tmp << 8) ^ ((uint16_t)tmp << 3) ^ (tmp >> 4));
    }
    return crc;
}

size_t radio_status_encode(const radio_status_t *rs, uint8_t version, uint8_t seq,
                           uint8_t *out, size_t cap)
{
    if (rs == NULL || out == NULL || (version != 1u && version != 2u)) {
        return 0;
    }

    /* Wire order: fields sorted by size (uint16 first). */
    uint8_t payload[RADIO_STATUS_LEN] = {
        (uint8_t)(rs->rxerrors & 0xFFu), (uint8_t)(rs->rxerrors >> 8),
        (uint8_t)(rs->fixed & 0xFFu),    (uint8_t)(rs->fixed >> 8),
        rs->rssi, rs->remrssi, rs->txbuf, rs->noise, rs->remnoise,
    };

    size_t plen = RADIO_STATUS_LEN;
    size_t hdr;
    if (version == 2u) {
        while (plen > 1u && payload[plen - 1u] == 0u) {
            plen--; /* MAVLink 2 payload truncation */
        }
        hdr = 10u;
    } else {
        hdr = 6u;
    }
    const size_t total = hdr + plen + 2u;
    if (cap < total) {
        return 0;
    }

    size_t i = 0;
    if (version == 2u) {
        out[i++] = 0xFDu;
        out[i++] = (uint8_t)plen;
        out[i++] = 0u; /* incompat flags: unsigned */
        out[i++] = 0u; /* compat flags */
        out[i++] = seq;
        out[i++] = RADIO_STATUS_SYSID;
        out[i++] = RADIO_STATUS_COMPID;
        out[i++] = RADIO_STATUS_MSGID & 0xFFu;
        out[i++] = 0u;
        out[i++] = 0u;
    } else {
        out[i++] = 0xFEu;
        out[i++] = (uint8_t)plen;
        out[i++] = seq;
        out[i++] = RADIO_STATUS_SYSID;
        out[i++] = RADIO_STATUS_COMPID;
        out[i++] = RADIO_STATUS_MSGID;
    }
    for (size_t k = 0; k < plen; k++) {
        out[i++] = payload[k];
    }

    uint16_t crc = mav_crc_x25(&out[1], i - 1u, 0xFFFFu);
    const uint8_t extra = RADIO_STATUS_CRC_EXTRA;
    crc = mav_crc_x25(&extra, 1u, crc);
    out[i++] = (uint8_t)(crc & 0xFFu);
    out[i++] = (uint8_t)(crc >> 8);
    return i;
}

uint8_t radio_status_dbm_to_sik(int dbm)
{
    int v = ((dbm + 127) * 19 + 5) / 10;
    if (v < 0) {
        v = 0;
    }
    if (v > 254) {
        v = 254;
    }
    return (uint8_t)v;
}
