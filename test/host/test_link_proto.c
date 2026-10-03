#include <string.h>

#include "link_proto.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* REQ-LNK-01: frames round-trip and every corruption class is rejected. */
static void test_frame_roundtrip(void)
{
    uint8_t payload[LINK_MAX_PAYLOAD];
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i * 7u);
    }
    uint8_t buf[LINK_MAX_FRAME];
    size_t n = link_frame_encode(buf, sizeof(buf), LINK_T_DATA, 0x1234, 0xBEEF, payload, LINK_MAX_PAYLOAD);
    TEST_ASSERT_EQUAL_UINT(LINK_MAX_FRAME, n);

    link_frame_t f;
    TEST_ASSERT_EQUAL(LINK_OK, link_frame_decode(buf, n, 0x1234, &f));
    TEST_ASSERT_EQUAL(LINK_T_DATA, f.type);
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, f.seq);
    TEST_ASSERT_EQUAL_UINT16(LINK_MAX_PAYLOAD, f.len);
    TEST_ASSERT_EQUAL_MEMORY(payload, f.payload, LINK_MAX_PAYLOAD);
}

static void test_frame_rejects(void)
{
    const uint8_t p[4] = {1, 2, 3, 4};
    uint8_t buf[32];
    link_frame_t f;
    size_t n = link_frame_encode(buf, sizeof(buf), LINK_T_HEARTBEAT, 7, 1, p, sizeof(p));
    TEST_ASSERT_EQUAL_UINT(LINK_HDR_LEN + 4u + LINK_CRC_LEN, n);

    TEST_ASSERT_EQUAL(LINK_E_SHORT, link_frame_decode(buf, 5, 7, &f));
    TEST_ASSERT_EQUAL(LINK_E_LEN, link_frame_decode(buf, n - 1u, 7, &f));
    TEST_ASSERT_EQUAL(LINK_E_NET, link_frame_decode(buf, n, 8, &f));

    uint8_t bad[32];
    for (size_t i = 0; i < n; i++) { /* flip every bit of every byte */
        for (int b = 0; b < 8; b++) {
            memcpy(bad, buf, n);
            bad[i] ^= (uint8_t)(1u << b);
            TEST_ASSERT_NOT_EQUAL(LINK_OK, link_frame_decode(bad, n, 7, &f));
        }
    }

    memcpy(bad, buf, n);
    bad[0] = 0x00;
    TEST_ASSERT_EQUAL(LINK_E_MAGIC, link_frame_decode(bad, n, 7, &f));
    memcpy(bad, buf, n);
    bad[1] = (uint8_t)((2u << 4) | LINK_T_HEARTBEAT);
    TEST_ASSERT_EQUAL(LINK_E_VERSION, link_frame_decode(bad, n, 7, &f));
}

static void test_frame_encode_guards(void)
{
    uint8_t buf[LINK_MAX_FRAME + 8u];
    uint8_t big[LINK_MAX_PAYLOAD + 1u] = {0};
    TEST_ASSERT_EQUAL_UINT(0, link_frame_encode(buf, sizeof(buf), LINK_T_DATA, 1, 1, big, sizeof(big)));
    TEST_ASSERT_EQUAL_UINT(0, link_frame_encode(buf, 10, LINK_T_DATA, 1, 1, big, 4));
    TEST_ASSERT_EQUAL_UINT(0, link_frame_encode(buf, sizeof(buf), (link_type_t)9, 1, 1, big, 4));
    TEST_ASSERT_EQUAL_UINT(0, link_frame_encode(buf, sizeof(buf), LINK_T_DATA, 1, 1, NULL, 4));
    TEST_ASSERT_EQUAL_UINT(LINK_HDR_LEN + LINK_CRC_LEN,
                           link_frame_encode(buf, sizeof(buf), LINK_T_DATA, 1, 1, NULL, 0));
}

static void test_heartbeat_roundtrip(void)
{
    link_hb_t a = {.state = LINK_DEGRADED, .rssi_dbm = -87, .noise_dbm = -95,
                   .txbuf_pct = 42, .rx_lost = 65535, .rx_errors = 3};
    uint8_t buf[LINK_HB_LEN];
    TEST_ASSERT_EQUAL_UINT(LINK_HB_LEN, link_hb_encode(&a, buf, sizeof(buf)));
    link_hb_t b;
    TEST_ASSERT_TRUE(link_hb_decode(buf, sizeof(buf), &b));
    TEST_ASSERT_EQUAL_INT8(-87, b.rssi_dbm);
    TEST_ASSERT_EQUAL_INT8(-95, b.noise_dbm);
    TEST_ASSERT_EQUAL_UINT8(42, b.txbuf_pct);
    TEST_ASSERT_EQUAL_UINT16(65535, b.rx_lost);
    TEST_ASSERT_FALSE(link_hb_decode(buf, sizeof(buf) - 1u, &b));
    buf[3] = 250; /* out-of-range percentage is clamped */
    TEST_ASSERT_TRUE(link_hb_decode(buf, sizeof(buf), &b));
    TEST_ASSERT_EQUAL_UINT8(100, b.txbuf_pct);
}

/* REQ-LNK-02: duplicates/replays inside the window are rejected, loss counted. */
static void test_seq_window(void)
{
    link_seq_t s;
    link_seq_reset(&s);
    TEST_ASSERT_EQUAL(LINK_SEQ_ACCEPT, link_seq_check(&s, 100));
    TEST_ASSERT_EQUAL(LINK_SEQ_ACCEPT, link_seq_check(&s, 101));
    TEST_ASSERT_EQUAL(LINK_SEQ_REJECT, link_seq_check(&s, 101));
    TEST_ASSERT_EQUAL(LINK_SEQ_REJECT, link_seq_check(&s, 50));
    TEST_ASSERT_EQUAL(LINK_SEQ_ACCEPT, link_seq_check(&s, 105));
    TEST_ASSERT_EQUAL_UINT32(3, s.lost);
    TEST_ASSERT_EQUAL_UINT32(2, s.dup);
}

static void test_seq_wrap_and_window(void)
{
    link_seq_t s;
    link_seq_reset(&s);
    TEST_ASSERT_EQUAL(LINK_SEQ_ACCEPT, link_seq_check(&s, 65534));
    TEST_ASSERT_EQUAL(LINK_SEQ_ACCEPT, link_seq_check(&s, 65535));
    TEST_ASSERT_EQUAL(LINK_SEQ_ACCEPT, link_seq_check(&s, 0));
    TEST_ASSERT_EQUAL(LINK_SEQ_ACCEPT, link_seq_check(&s, 2));
    TEST_ASSERT_EQUAL_UINT32(1, s.lost);
    TEST_ASSERT_EQUAL(LINK_SEQ_REJECT, link_seq_check(&s, 65535));

    /* Replays / wild jumps are never accepted inside a session. */
    TEST_ASSERT_EQUAL(LINK_SEQ_REJECT, link_seq_check(&s, (uint16_t)(2 + LINK_SEQ_WINDOW)));
    TEST_ASSERT_EQUAL(LINK_SEQ_REJECT, link_seq_check(&s, (uint16_t)(2 - 5000)));
    TEST_ASSERT_EQUAL(LINK_SEQ_ACCEPT, link_seq_check(&s, (uint16_t)(2 + LINK_SEQ_WINDOW - 1)));

    /* A new session starts a fresh window. */
    link_seq_reset(&s);
    TEST_ASSERT_EQUAL(LINK_SEQ_ACCEPT, link_seq_check(&s, 0));
    TEST_ASSERT_EQUAL(LINK_SEQ_ACCEPT, link_seq_check(&s, 1));
}

/* REQ-LNK-03: link state transitions and timing. */
static void test_state_machine(void)
{
    link_sm_t sm;
    link_sm_init(&sm, 300, 1500);
    TEST_ASSERT_EQUAL(LINK_SEARCHING, sm.state);
    TEST_ASSERT_EQUAL(LINK_EV_NONE, link_sm_tick(&sm, 10000));

    TEST_ASSERT_EQUAL(LINK_EV_CONNECTED, link_sm_on_peer_rx(&sm, 1000));
    TEST_ASSERT_TRUE(link_sm_is_up(&sm));
    TEST_ASSERT_EQUAL(LINK_EV_NONE, link_sm_tick(&sm, 1299));
    TEST_ASSERT_EQUAL(LINK_EV_DEGRADED, link_sm_tick(&sm, 1300));
    TEST_ASSERT_EQUAL(LINK_EV_NONE, link_sm_tick(&sm, 1400));
    TEST_ASSERT_EQUAL(LINK_EV_RECOVERED, link_sm_on_peer_rx(&sm, 1400));
    TEST_ASSERT_EQUAL(LINK_CONNECTED, sm.state);

    TEST_ASSERT_EQUAL(LINK_EV_DEGRADED, link_sm_tick(&sm, 1800));
    TEST_ASSERT_EQUAL(LINK_EV_LOST, link_sm_tick(&sm, 2900));
    TEST_ASSERT_EQUAL(LINK_SEARCHING, sm.state);
    TEST_ASSERT_EQUAL_UINT32(1, sm.losses);
    TEST_ASSERT_EQUAL(LINK_EV_NONE, link_sm_force_lost(&sm));
}

static void test_state_machine_time_wrap(void)
{
    link_sm_t sm;
    link_sm_init(&sm, 300, 1500);
    link_sm_on_peer_rx(&sm, 0xFFFFFF00u);
    TEST_ASSERT_EQUAL(LINK_EV_NONE, link_sm_tick(&sm, 0x00000010u)); /* 272 ms later */
    TEST_ASSERT_EQUAL(LINK_EV_DEGRADED, link_sm_tick(&sm, 0x00000030u));
    TEST_ASSERT_EQUAL(LINK_EV_LOST, link_sm_tick(&sm, 0x00000600u));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_frame_roundtrip);
    RUN_TEST(test_frame_rejects);
    RUN_TEST(test_frame_encode_guards);
    RUN_TEST(test_heartbeat_roundtrip);
    RUN_TEST(test_seq_window);
    RUN_TEST(test_seq_wrap_and_window);
    RUN_TEST(test_state_machine);
    RUN_TEST(test_state_machine_time_wrap);
    return UNITY_END();
}
