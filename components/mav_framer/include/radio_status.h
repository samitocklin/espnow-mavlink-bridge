/*
 * radio_status - encoder for MAVLink RADIO_STATUS (#109), emitted the same
 * way SiK radios do (sysid 51, compid 68 = MAV_COMP_ID_TELEMETRY_RADIO) so
 * ArduPilot's stream throttling and GCS link-quality displays work unchanged.
 * Pure C99 (host unit-testable).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RADIO_STATUS_MSGID     109u
#define RADIO_STATUS_CRC_EXTRA 185u
#define RADIO_STATUS_LEN       9u
#define RADIO_STATUS_SYSID     51u
#define RADIO_STATUS_COMPID    68u
#define RADIO_STATUS_MAX_BYTES (12u + RADIO_STATUS_LEN)

typedef struct {
    uint8_t  rssi;      /* SiK scale, see radio_status_dbm_to_sik() */
    uint8_t  remrssi;
    uint8_t  txbuf;     /* free TX buffer, percent */
    uint8_t  noise;
    uint8_t  remnoise;
    uint16_t rxerrors;
    uint16_t fixed;
} radio_status_t;

uint16_t mav_crc_x25(const uint8_t *data, size_t len, uint16_t crc);

/* Encode as MAVLink v1 (version 1) or v2 (version 2, unsigned, payload
 * trailing-zero truncated). Returns bytes written or 0 on error. */
size_t radio_status_encode(const radio_status_t *rs, uint8_t version, uint8_t seq,
                           uint8_t *out, size_t cap);

/* SiK convention, which Mission Planner reverses as dBm = v / 1.9 - 127. */
uint8_t radio_status_dbm_to_sik(int dbm);

#ifdef __cplusplus
}
#endif
