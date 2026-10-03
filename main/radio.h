/*
 * radio - ESP-NOW transport: Wi-Fi bring-up, authenticated pairing, link
 * supervision (heartbeats, state machine) and the air TX task.
 *
 * Threading: the ESP-NOW callbacks only copy into queues. radio_rx_process()
 * is called by exactly one task (the serial writer); the TX task is internal.
 * Shared link state is guarded by one mutex.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "link_proto.h"
#include "pairing.h"

/* Largest frame on air: link frame + per-frame session tag. */
#define RADIO_MAX_AIR (LINK_MAX_FRAME + PAIR_FRAME_TAG_LEN)

typedef struct {
    uint8_t  src[6];
    uint8_t  dst[6];
    int8_t   rssi;
    int8_t   noise;
    uint16_t len;
    uint8_t  data[RADIO_MAX_AIR];
} radio_rx_t;

typedef struct {
    link_state_t state;
    bool         peer_valid;
    uint8_t      peer_mac[6];
    int8_t       rssi;       /* last frame from peer, dBm */
    int8_t       noise;
    int8_t       rem_rssi;   /* as reported by the peer */
    int8_t       rem_noise;
    uint8_t      rem_txbuf;
    uint8_t      txbuf_pct;  /* local air-TX queue free, percent */
    uint32_t     rx_lost;
    uint32_t     rx_dup;
    uint32_t     rx_data;
    uint32_t     tx_data;
    uint32_t     losses;
    int8_t       tx_power_qdbm; /* applied max TX power, 0.25 dBm units */
} radio_snapshot_t;

/* Brings up Wi-Fi + ESP-NOW. Transient failures are retried, then restart.
 * Returns false for a configuration error (country / channel not allowed),
 * in which case the radio is left off. */
bool radio_init(pair_role_t role, const pair_keys_t *keys, uint16_t net_id);
void radio_start(void);

/* Queue opaque serial bytes for the peer. Drops (and counts) when the link is
 * down; when the queue is full the oldest frame is dropped. */
bool radio_submit(const uint8_t *data, size_t len);

bool   radio_rx_wait(radio_rx_t *out, uint32_t timeout_ms);
/* Validates and handles one received frame. Returns the DATA payload length
 * (payload points into `in`), or 0 for control / rejected frames. */
size_t radio_rx_process(const radio_rx_t *in, const uint8_t **payload);

void     radio_snapshot(radio_snapshot_t *out);
/* Incremented every time the link is lost; consumers discard buffered data
 * when it changes so nothing stale is delivered after a reconnect. */
uint32_t radio_link_epoch(void);
