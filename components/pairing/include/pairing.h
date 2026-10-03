/*
 * pairing - shared-key authentication for automatic peer discovery.
 *
 * Both units are built with the same 128-bit key and network id. From it we
 * derive (HMAC-SHA256, domain-separated):
 *   - PMK / LMK for ESP-NOW's built-in CCMP encryption of unicast traffic
 *   - a beacon key used to authenticate the broadcast pairing beacons
 *   - a data key: every unicast frame carries a truncated HMAC bound to the
 *     session (both handshake nonces). ESP-NOW does not document that it
 *     rejects *unencrypted* unicast from an encrypted peer, so CCMP is used for
 *     confidentiality only and authenticity never depends on it.
 *
 * Handshake: each unit broadcasts BEACON{role, mac, nonce, echo}. `echo` is the
 * nonce most recently heard from an opposite-role unit. A unit accepts a peer
 * only when that peer's beacon is authentic AND echoes the unit's own current
 * nonce, which proves the peer is live (not a replayed capture).
 *
 * Depends only on mbedtls SHA-256 (host unit-testable).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAIR_KEY_LEN     16u
#define PAIR_TAG_LEN     16u
#define PAIR_BEACON_LEN  (1u + 6u + 4u + 4u + PAIR_TAG_LEN)
#define PAIR_FRAME_TAG_LEN 16u

typedef enum {
    PAIR_ROLE_GROUND = 1,
    PAIR_ROLE_AIR    = 2,
} pair_role_t;

typedef struct {
    uint8_t pmk[16];
    uint8_t lmk[16];
    uint8_t beacon_key[32];
    uint8_t data_key[32];
} pair_keys_t;

/* Identifies one pairing session; frames from other sessions fail auth. */
typedef struct {
    uint32_t ground_nonce;
    uint32_t air_nonce;
} pair_session_t;

typedef struct {
    uint8_t  role;
    uint8_t  mac[6];
    uint32_t nonce;
    uint32_t echo;
} pair_beacon_t;

typedef enum {
    PAIR_OK = 0,
    PAIR_E_LEN,
    PAIR_E_AUTH, /* tag mismatch: wrong key or tampered */
    PAIR_E_ROLE, /* unknown role value */
} pair_err_t;

void pair_hmac_sha256(const uint8_t *key, size_t key_len,
                 const uint8_t *msg, size_t msg_len, uint8_t out[32]);

/* Parse exactly 32 hex characters. Rejects the all-zero key. */
bool pair_parse_hex_key(const char *hex, uint8_t key[PAIR_KEY_LEN]);

void pair_derive_keys(const uint8_t key[PAIR_KEY_LEN], uint16_t net_id, pair_keys_t *out);

/* Returns PAIR_BEACON_LEN on success, 0 if cap is too small. */
size_t pair_beacon_encode(const pair_keys_t *k, uint16_t net_id,
                          const pair_beacon_t *b, uint8_t *out, size_t cap);

pair_err_t pair_beacon_decode(const pair_keys_t *k, uint16_t net_id,
                              const uint8_t *in, size_t len, pair_beacon_t *b);

pair_role_t pair_opposite_role(pair_role_t r);

void pair_frame_tag(const pair_keys_t *k, const pair_session_t *sess,
                    const uint8_t *frame, size_t len, uint8_t tag[PAIR_FRAME_TAG_LEN]);

/* Constant-time comparison against the expected tag. */
bool pair_frame_verify(const pair_keys_t *k, const pair_session_t *sess,
                       const uint8_t *frame, size_t len, const uint8_t tag[PAIR_FRAME_TAG_LEN]);

#ifdef __cplusplus
}
#endif
