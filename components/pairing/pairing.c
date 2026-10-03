#include "pairing.h"

#include <string.h>

#include "mbedtls/sha256.h"

#define SHA256_BLOCK 64u

void pair_hmac_sha256(const uint8_t *key, size_t key_len,
                 const uint8_t *msg, size_t msg_len, uint8_t out[32])
{
    uint8_t k0[SHA256_BLOCK] = {0};
    uint8_t pad[SHA256_BLOCK];
    uint8_t inner[32];
    mbedtls_sha256_context ctx;

    if (key_len > SHA256_BLOCK) {
        (void)mbedtls_sha256(key, key_len, k0, 0);
    } else {
        memcpy(k0, key, key_len);
    }

    mbedtls_sha256_init(&ctx);
    for (size_t i = 0; i < SHA256_BLOCK; i++) {
        pad[i] = (uint8_t)(k0[i] ^ 0x36u);
    }
    (void)mbedtls_sha256_starts(&ctx, 0);
    (void)mbedtls_sha256_update(&ctx, pad, SHA256_BLOCK);
    (void)mbedtls_sha256_update(&ctx, msg, msg_len);
    (void)mbedtls_sha256_finish(&ctx, inner);

    for (size_t i = 0; i < SHA256_BLOCK; i++) {
        pad[i] = (uint8_t)(k0[i] ^ 0x5Cu);
    }
    (void)mbedtls_sha256_starts(&ctx, 0);
    (void)mbedtls_sha256_update(&ctx, pad, SHA256_BLOCK);
    (void)mbedtls_sha256_update(&ctx, inner, sizeof(inner));
    (void)mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);

    memset(k0, 0, sizeof(k0));
    memset(pad, 0, sizeof(pad));
    memset(inner, 0, sizeof(inner));
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool pair_parse_hex_key(const char *hex, uint8_t key[PAIR_KEY_LEN])
{
    if (hex == NULL || strlen(hex) != PAIR_KEY_LEN * 2u) {
        return false;
    }
    uint8_t acc = 0;
    for (size_t i = 0; i < PAIR_KEY_LEN; i++) {
        const int hi = hex_nibble(hex[2u * i]);
        const int lo = hex_nibble(hex[2u * i + 1u]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        key[i] = (uint8_t)((hi << 4) | lo);
        acc |= key[i];
    }
    return acc != 0u;
}

static void derive(const uint8_t key[PAIR_KEY_LEN], uint16_t net_id, const char *label,
                   uint8_t *out, size_t out_len)
{
    uint8_t msg[16] = {0};
    const size_t llen = strlen(label);
    memcpy(msg, label, llen);
    msg[llen]      = (uint8_t)(net_id & 0xFFu);
    msg[llen + 1u] = (uint8_t)(net_id >> 8);

    uint8_t full[32];
    pair_hmac_sha256(key, PAIR_KEY_LEN, msg, llen + 2u, full);
    memcpy(out, full, out_len);
    memset(full, 0, sizeof(full));
}

void pair_derive_keys(const uint8_t key[PAIR_KEY_LEN], uint16_t net_id, pair_keys_t *out)
{
    derive(key, net_id, "emb-pmk", out->pmk, sizeof(out->pmk));
    derive(key, net_id, "emb-lmk", out->lmk, sizeof(out->lmk));
    derive(key, net_id, "emb-bcn", out->beacon_key, sizeof(out->beacon_key));
    derive(key, net_id, "emb-dat", out->data_key, sizeof(out->data_key));
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Tag covers the network id and every beacon field. */
static void beacon_tag(const pair_keys_t *k, uint16_t net_id, const uint8_t *body,
                       uint8_t tag[PAIR_TAG_LEN])
{
    uint8_t msg[2u + PAIR_BEACON_LEN - PAIR_TAG_LEN];
    msg[0] = (uint8_t)(net_id & 0xFFu);
    msg[1] = (uint8_t)(net_id >> 8);
    memcpy(&msg[2], body, PAIR_BEACON_LEN - PAIR_TAG_LEN);
    uint8_t full[32];
    pair_hmac_sha256(k->beacon_key, sizeof(k->beacon_key), msg, sizeof(msg), full);
    memcpy(tag, full, PAIR_TAG_LEN);
}

size_t pair_beacon_encode(const pair_keys_t *k, uint16_t net_id,
                          const pair_beacon_t *b, uint8_t *out, size_t cap)
{
    if (k == NULL || b == NULL || out == NULL || cap < PAIR_BEACON_LEN) {
        return 0;
    }
    out[0] = b->role;
    memcpy(&out[1], b->mac, 6);
    put_u32(&out[7], b->nonce);
    put_u32(&out[11], b->echo);
    beacon_tag(k, net_id, out, &out[15]);
    return PAIR_BEACON_LEN;
}

pair_err_t pair_beacon_decode(const pair_keys_t *k, uint16_t net_id,
                              const uint8_t *in, size_t len, pair_beacon_t *b)
{
    if (k == NULL || in == NULL || b == NULL || len != PAIR_BEACON_LEN) {
        return PAIR_E_LEN;
    }
    uint8_t tag[PAIR_TAG_LEN];
    beacon_tag(k, net_id, in, tag);
    uint8_t diff = 0;
    for (size_t i = 0; i < PAIR_TAG_LEN; i++) {
        diff |= (uint8_t)(tag[i] ^ in[15u + i]); /* constant time */
    }
    if (diff != 0u) {
        return PAIR_E_AUTH;
    }
    if (in[0] != PAIR_ROLE_GROUND && in[0] != PAIR_ROLE_AIR) {
        return PAIR_E_ROLE;
    }
    b->role = in[0];
    memcpy(b->mac, &in[1], 6);
    b->nonce = get_u32(&in[7]);
    b->echo  = get_u32(&in[11]);
    return PAIR_OK;
}

pair_role_t pair_opposite_role(pair_role_t r)
{
    return r == PAIR_ROLE_GROUND ? PAIR_ROLE_AIR : PAIR_ROLE_GROUND;
}

void pair_frame_tag(const pair_keys_t *k, const pair_session_t *sess,
                    const uint8_t *frame, size_t len, uint8_t tag[PAIR_FRAME_TAG_LEN])
{
    /* HMAC(data_key, ground_nonce | air_nonce | frame): two-pass SHA-256 with
     * the session prefix fed first, so the frame is never copied. */
    uint8_t k0[SHA256_BLOCK] = {0};
    uint8_t pad[SHA256_BLOCK];
    uint8_t sid[8];
    uint8_t inner[32];
    uint8_t full[32];
    mbedtls_sha256_context ctx;

    memcpy(k0, k->data_key, sizeof(k->data_key));
    put_u32(&sid[0], sess->ground_nonce);
    put_u32(&sid[4], sess->air_nonce);

    mbedtls_sha256_init(&ctx);
    for (size_t i = 0; i < SHA256_BLOCK; i++) {
        pad[i] = (uint8_t)(k0[i] ^ 0x36u);
    }
    (void)mbedtls_sha256_starts(&ctx, 0);
    (void)mbedtls_sha256_update(&ctx, pad, SHA256_BLOCK);
    (void)mbedtls_sha256_update(&ctx, sid, sizeof(sid));
    (void)mbedtls_sha256_update(&ctx, frame, len);
    (void)mbedtls_sha256_finish(&ctx, inner);
    for (size_t i = 0; i < SHA256_BLOCK; i++) {
        pad[i] = (uint8_t)(k0[i] ^ 0x5Cu);
    }
    (void)mbedtls_sha256_starts(&ctx, 0);
    (void)mbedtls_sha256_update(&ctx, pad, SHA256_BLOCK);
    (void)mbedtls_sha256_update(&ctx, inner, sizeof(inner));
    (void)mbedtls_sha256_finish(&ctx, full);
    mbedtls_sha256_free(&ctx);

    memcpy(tag, full, PAIR_FRAME_TAG_LEN);
    memset(k0, 0, sizeof(k0));
    memset(pad, 0, sizeof(pad));
}

bool pair_frame_verify(const pair_keys_t *k, const pair_session_t *sess,
                       const uint8_t *frame, size_t len, const uint8_t tag[PAIR_FRAME_TAG_LEN])
{
    uint8_t expect[PAIR_FRAME_TAG_LEN];
    pair_frame_tag(k, sess, frame, len, expect);
    uint8_t diff = 0;
    for (size_t i = 0; i < PAIR_FRAME_TAG_LEN; i++) {
        diff |= (uint8_t)(expect[i] ^ tag[i]);
    }
    return diff == 0u;
}
