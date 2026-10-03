#include <stdio.h>
#include <string.h>

#include "pairing.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void hex(const char *s, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        TEST_ASSERT_EQUAL_INT(1, sscanf(&s[2 * i], "%2x", &v));
        out[i] = (uint8_t)v;
    }
}

/* RFC 4231 test cases 1, 2 and 6 (key longer than block size). */
static void test_hmac_rfc4231(void)
{
    uint8_t out[32], exp[32];

    uint8_t k1[20];
    memset(k1, 0x0B, sizeof(k1));
    pair_hmac_sha256(k1, sizeof(k1), (const uint8_t *)"Hi There", 8, out);
    hex("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", exp, 32);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(exp, out, 32);

    const char *d2 = "what do ya want for nothing?";
    pair_hmac_sha256((const uint8_t *)"Jefe", 4, (const uint8_t *)d2, strlen(d2), out);
    hex("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", exp, 32);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(exp, out, 32);

    uint8_t k6[131];
    memset(k6, 0xAA, sizeof(k6));
    const char *d6 = "Test Using Larger Than Block-Size Key - Hash Key First";
    pair_hmac_sha256(k6, sizeof(k6), (const uint8_t *)d6, strlen(d6), out);
    hex("60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", exp, 32);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(exp, out, 32);
}

/* REQ-SEC-03: invalid or placeholder keys are refused. */
static void test_key_parse(void)
{
    uint8_t k[PAIR_KEY_LEN];
    TEST_ASSERT_TRUE(pair_parse_hex_key("00112233445566778899aabbccddEEFF", k));
    TEST_ASSERT_EQUAL_HEX8(0xFF, k[15]);
    TEST_ASSERT_FALSE(pair_parse_hex_key("00000000000000000000000000000000", k));
    TEST_ASSERT_FALSE(pair_parse_hex_key("CHANGE_ME", k));
    TEST_ASSERT_FALSE(pair_parse_hex_key("00112233445566778899aabbccddeef", k));
    TEST_ASSERT_FALSE(pair_parse_hex_key("00112233445566778899aabbccddeeff0", k));
    TEST_ASSERT_FALSE(pair_parse_hex_key("0011223344556677889Xaabbccddeeff", k));
    TEST_ASSERT_FALSE(pair_parse_hex_key(NULL, k));
}

static void test_key_derivation_separation(void)
{
    uint8_t k[PAIR_KEY_LEN];
    TEST_ASSERT_TRUE(pair_parse_hex_key("00112233445566778899aabbccddeeff", k));
    pair_keys_t a, b;
    pair_derive_keys(k, 1, &a);
    pair_derive_keys(k, 2, &b);
    TEST_ASSERT_TRUE(memcmp(a.pmk, a.lmk, 16) != 0);
    TEST_ASSERT_TRUE(memcmp(a.pmk, b.pmk, 16) != 0);
    TEST_ASSERT_TRUE(memcmp(a.lmk, b.lmk, 16) != 0);
    TEST_ASSERT_TRUE(memcmp(a.beacon_key, a.data_key, 32) != 0);
    pair_derive_keys(k, 1, &b);
    TEST_ASSERT_EQUAL_MEMORY(&a, &b, sizeof(a)); /* deterministic on both units */
}

/* REQ-SEC-01: beacons authenticate; wrong key, net id or any tampering fails. */
static void test_beacon_auth(void)
{
    uint8_t k[PAIR_KEY_LEN], k2[PAIR_KEY_LEN];
    TEST_ASSERT_TRUE(pair_parse_hex_key("00112233445566778899aabbccddeeff", k));
    TEST_ASSERT_TRUE(pair_parse_hex_key("00112233445566778899aabbccddeefe", k2));
    pair_keys_t keys, other;
    pair_derive_keys(k, 0x0042, &keys);
    pair_derive_keys(k2, 0x0042, &other);

    const pair_beacon_t b = {.role = PAIR_ROLE_AIR, .mac = {1, 2, 3, 4, 5, 6},
                             .nonce = 0xDEADBEEF, .echo = 0x01020304};
    uint8_t buf[PAIR_BEACON_LEN];
    TEST_ASSERT_EQUAL_UINT(PAIR_BEACON_LEN, pair_beacon_encode(&keys, 0x0042, &b, buf, sizeof(buf)));

    pair_beacon_t d;
    TEST_ASSERT_EQUAL(PAIR_OK, pair_beacon_decode(&keys, 0x0042, buf, sizeof(buf), &d));
    TEST_ASSERT_EQUAL_UINT8(PAIR_ROLE_AIR, d.role);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(b.mac, d.mac, 6);
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEF, d.nonce);
    TEST_ASSERT_EQUAL_HEX32(0x01020304, d.echo);

    TEST_ASSERT_EQUAL(PAIR_E_AUTH, pair_beacon_decode(&other, 0x0042, buf, sizeof(buf), &d));
    TEST_ASSERT_EQUAL(PAIR_E_AUTH, pair_beacon_decode(&keys, 0x0043, buf, sizeof(buf), &d));
    TEST_ASSERT_EQUAL(PAIR_E_LEN, pair_beacon_decode(&keys, 0x0042, buf, sizeof(buf) - 1u, &d));

    for (size_t i = 0; i < sizeof(buf); i++) {
        uint8_t t[PAIR_BEACON_LEN];
        memcpy(t, buf, sizeof(t));
        t[i] ^= 0x01;
        TEST_ASSERT_EQUAL(PAIR_E_AUTH, pair_beacon_decode(&keys, 0x0042, t, sizeof(t), &d));
    }
    TEST_ASSERT_EQUAL_UINT(0, pair_beacon_encode(&keys, 0x0042, &b, buf, sizeof(buf) - 1u));
}

static void test_opposite_role(void)
{
    TEST_ASSERT_EQUAL(PAIR_ROLE_AIR, pair_opposite_role(PAIR_ROLE_GROUND));
    TEST_ASSERT_EQUAL(PAIR_ROLE_GROUND, pair_opposite_role(PAIR_ROLE_AIR));
}

/* The session-bound tag equals a plain HMAC over (nonces | frame). */
static void test_frame_tag_is_hmac(void)
{
    uint8_t k[PAIR_KEY_LEN];
    TEST_ASSERT_TRUE(pair_parse_hex_key("00112233445566778899aabbccddeeff", k));
    pair_keys_t keys;
    pair_derive_keys(k, 7, &keys);
    const pair_session_t sess = {.ground_nonce = 0x11223344, .air_nonce = 0xAABBCCDD};
    uint8_t frame[300];
    for (size_t i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i ^ 0x5A);
    }
    uint8_t msg[8 + sizeof(frame)] = {0x44, 0x33, 0x22, 0x11, 0xDD, 0xCC, 0xBB, 0xAA};
    memcpy(&msg[8], frame, sizeof(frame));
    uint8_t ref[32], tag[PAIR_FRAME_TAG_LEN];
    pair_hmac_sha256(keys.data_key, sizeof(keys.data_key), msg, sizeof(msg), ref);
    pair_frame_tag(&keys, &sess, frame, sizeof(frame), tag);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ref, tag, PAIR_FRAME_TAG_LEN);
}

/* REQ-SEC-02: unicast frames are bound to key and session; any change fails. */
static void test_frame_tag_rejects(void)
{
    uint8_t k[PAIR_KEY_LEN], k2[PAIR_KEY_LEN];
    TEST_ASSERT_TRUE(pair_parse_hex_key("00112233445566778899aabbccddeeff", k));
    TEST_ASSERT_TRUE(pair_parse_hex_key("ff112233445566778899aabbccddeeff", k2));
    pair_keys_t keys, other;
    pair_derive_keys(k, 7, &keys);
    pair_derive_keys(k2, 7, &other);
    const pair_session_t sess = {.ground_nonce = 1, .air_nonce = 2};
    const pair_session_t old  = {.ground_nonce = 1, .air_nonce = 3};
    uint8_t frame[40];
    memset(frame, 0x77, sizeof(frame));
    uint8_t tag[PAIR_FRAME_TAG_LEN];
    pair_frame_tag(&keys, &sess, frame, sizeof(frame), tag);

    TEST_ASSERT_TRUE(pair_frame_verify(&keys, &sess, frame, sizeof(frame), tag));
    TEST_ASSERT_FALSE(pair_frame_verify(&keys, &old, frame, sizeof(frame), tag));
    TEST_ASSERT_FALSE(pair_frame_verify(&other, &sess, frame, sizeof(frame), tag));
    TEST_ASSERT_FALSE(pair_frame_verify(&keys, &sess, frame, sizeof(frame) - 1u, tag));
    for (size_t i = 0; i < sizeof(frame); i++) {
        frame[i] ^= 0x80;
        TEST_ASSERT_FALSE(pair_frame_verify(&keys, &sess, frame, sizeof(frame), tag));
        frame[i] ^= 0x80;
    }
    tag[PAIR_FRAME_TAG_LEN - 1] ^= 1;
    TEST_ASSERT_FALSE(pair_frame_verify(&keys, &sess, frame, sizeof(frame), tag));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_hmac_rfc4231);
    RUN_TEST(test_key_parse);
    RUN_TEST(test_key_derivation_separation);
    RUN_TEST(test_beacon_auth);
    RUN_TEST(test_opposite_role);
    RUN_TEST(test_frame_tag_is_hmac);
    RUN_TEST(test_frame_tag_rejects);
    return UNITY_END();
}
