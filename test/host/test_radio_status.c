#include "radio_status.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* Reference bytes generated with pymavlink 2.x (common dialect), seq=7. */
static const radio_status_t RS = {.rssi = 100, .remrssi = 90, .txbuf = 80, .noise = 30,
                                  .remnoise = 25, .rxerrors = 3, .fixed = 0};

/* REQ-RS-01: RADIO_STATUS bytes identical to pymavlink's encoder. */
static void test_v1_matches_pymavlink(void)
{
    const uint8_t expect[] = {0xFE, 0x09, 0x07, 0x33, 0x44, 0x6D, 0x03, 0x00, 0x00,
                              0x00, 0x64, 0x5A, 0x50, 0x1E, 0x19, 0x38, 0x71};
    uint8_t out[RADIO_STATUS_MAX_BYTES];
    TEST_ASSERT_EQUAL_UINT(sizeof(expect), radio_status_encode(&RS, 1, 7, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expect, out, sizeof(expect));
}

static void test_v2_matches_pymavlink(void)
{
    const uint8_t expect[] = {0xFD, 0x09, 0x00, 0x00, 0x07, 0x33, 0x44, 0x6D, 0x00, 0x00, 0x03,
                              0x00, 0x00, 0x00, 0x64, 0x5A, 0x50, 0x1E, 0x19, 0x3B, 0x82};
    uint8_t out[RADIO_STATUS_MAX_BYTES];
    TEST_ASSERT_EQUAL_UINT(sizeof(expect), radio_status_encode(&RS, 2, 7, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expect, out, sizeof(expect));
}

static void test_v2_truncation_matches_pymavlink(void)
{
    radio_status_t rs = RS;
    rs.noise = 0;
    rs.remnoise = 0;
    const uint8_t expect[] = {0xFD, 0x07, 0x00, 0x00, 0x07, 0x33, 0x44, 0x6D, 0x00, 0x00,
                              0x03, 0x00, 0x00, 0x00, 0x64, 0x5A, 0x50, 0xC5, 0x76};
    uint8_t out[RADIO_STATUS_MAX_BYTES];
    TEST_ASSERT_EQUAL_UINT(sizeof(expect), radio_status_encode(&rs, 2, 7, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expect, out, sizeof(expect));

    const uint8_t v1[] = {0xFE, 0x09, 0x07, 0x33, 0x44, 0x6D, 0x03, 0x00, 0x00,
                          0x00, 0x64, 0x5A, 0x50, 0x00, 0x00, 0x3F, 0xA6};
    TEST_ASSERT_EQUAL_UINT(sizeof(v1), radio_status_encode(&rs, 1, 7, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(v1, out, sizeof(v1));
}

static void test_encode_guards(void)
{
    uint8_t out[RADIO_STATUS_MAX_BYTES];
    TEST_ASSERT_EQUAL_UINT(0, radio_status_encode(&RS, 3, 0, out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT(0, radio_status_encode(&RS, 2, 0, out, 20));
    TEST_ASSERT_EQUAL_UINT(0, radio_status_encode(NULL, 2, 0, out, sizeof(out)));
}

/* Mission Planner shows dBm = v / 1.9 - 127. */
static void test_dbm_scale(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, radio_status_dbm_to_sik(-127));
    TEST_ASSERT_EQUAL_UINT8(0, radio_status_dbm_to_sik(-200));
    TEST_ASSERT_EQUAL_UINT8(127, radio_status_dbm_to_sik(-60));
    TEST_ASSERT_EQUAL_UINT8(241, radio_status_dbm_to_sik(0));
    TEST_ASSERT_EQUAL_UINT8(254, radio_status_dbm_to_sik(20));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_v1_matches_pymavlink);
    RUN_TEST(test_v2_matches_pymavlink);
    RUN_TEST(test_v2_truncation_matches_pymavlink);
    RUN_TEST(test_encode_guards);
    RUN_TEST(test_dbm_scale);
    return UNITY_END();
}
