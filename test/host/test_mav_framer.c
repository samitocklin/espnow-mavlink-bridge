#include <stdlib.h>
#include <string.h>

#include "mav_framer.h"
#include "unity.h"

#define MAX_FRAMES 4096

typedef struct {
    uint8_t  data[MAX_FRAMES * 64];
    size_t   total;
    size_t   frame_len[MAX_FRAMES];
    size_t   frame_off[MAX_FRAMES];
    size_t   n;
} sink_t;

static sink_t sink;

static void emit(void *ctx, const uint8_t *d, size_t len)
{
    sink_t *s = ctx;
    TEST_ASSERT_TRUE(s->n < MAX_FRAMES);
    TEST_ASSERT_TRUE(s->total + len <= sizeof(s->data));
    s->frame_off[s->n] = s->total;
    s->frame_len[s->n] = len;
    memcpy(&s->data[s->total], d, len);
    s->total += len;
    s->n++;
}

void setUp(void) { memset(&sink, 0, sizeof(sink)); }
void tearDown(void) {}

/* Build a syntactically valid packet (CRC content irrelevant to the framer). */
static size_t make_v1(uint8_t *out, uint8_t plen, uint8_t fill)
{
    size_t i = 0;
    out[i++] = MAV_STX_V1;
    out[i++] = plen;
    for (int k = 0; k < 4 + plen + 2; k++) {
        out[i++] = fill;
    }
    return i;
}

static size_t make_v2(uint8_t *out, uint8_t plen, bool signed_pkt, uint8_t fill)
{
    size_t i = 0;
    out[i++] = MAV_STX_V2;
    out[i++] = plen;
    out[i++] = signed_pkt ? 0x01u : 0x00u;
    for (int k = 0; k < 7 + plen + 2 + (signed_pkt ? 13 : 0); k++) {
        out[i++] = fill;
    }
    return i;
}

static void test_scan_lengths(void)
{
    mav_scan_t s;
    struct { size_t len; } cases[3];
    uint8_t pk[3][MAV_MAX_PACKET];
    cases[0].len = make_v1(pk[0], 9, 0x11);
    cases[1].len = make_v2(pk[1], 255, false, 0x22);
    cases[2].len = make_v2(pk[2], 255, true, 0x33);
    TEST_ASSERT_EQUAL_UINT(17, cases[0].len);
    TEST_ASSERT_EQUAL_UINT(267, cases[1].len);
    TEST_ASSERT_EQUAL_UINT(MAV_MAX_PACKET, cases[2].len);

    for (int c = 0; c < 3; c++) {
        mav_scan_reset(&s);
        for (size_t i = 0; i + 1 < cases[c].len; i++) {
            TEST_ASSERT_EQUAL(MAV_SCAN_IN_PKT, mav_scan_byte(&s, pk[c][i]));
        }
        TEST_ASSERT_EQUAL(MAV_SCAN_PKT_END, mav_scan_byte(&s, pk[c][cases[c].len - 1]));
        TEST_ASSERT_TRUE(mav_scan_at_boundary(&s));
    }
    TEST_ASSERT_EQUAL(MAV_SCAN_RAW, mav_scan_byte(&s, 0x00));
}

/* REQ-SER-02: a complete packet waits for the hold time, then goes out whole. */
static void test_poll_hold_time(void)
{
    mav_framer_t f;
    mav_framer_init(&f, 250);
    TEST_ASSERT_EQUAL_UINT16(MAV_MAX_PACKET, f.out_cap); /* clamped up */

    uint8_t p[64];
    size_t n = make_v2(p, 20, false, 0x5A);
    for (size_t i = 0; i < n; i++) { /* byte-by-byte */
        mav_framer_push(&f, &p[i], 1, 100, emit, &sink);
    }
    TEST_ASSERT_EQUAL_UINT(0, sink.n);
    mav_framer_poll(&f, 104, 5, emit, &sink);
    TEST_ASSERT_EQUAL_UINT(0, sink.n);
    mav_framer_poll(&f, 105, 5, emit, &sink);
    TEST_ASSERT_EQUAL_UINT(1, sink.n);
    TEST_ASSERT_EQUAL_UINT(n, sink.frame_len[0]);
    TEST_ASSERT_EQUAL_MEMORY(p, sink.data, n);
    TEST_ASSERT_EQUAL_UINT8(2, f.last_version);
}

/* REQ-SER-01: packets are never split across radio frames. */
static void test_no_split(void)
{
    mav_framer_t f;
    mav_framer_init(&f, MAV_FRAMER_MAX_OUT);
    uint8_t stream[4096];
    size_t len = 0;
    size_t pkt_ends[64];
    size_t np = 0;
    for (int i = 0; i < 20; i++) {
        len += make_v2(&stream[len], (uint8_t)(40 + i * 9), (i % 3) == 0, (uint8_t)i);
        pkt_ends[np++] = len;
    }
    mav_framer_push(&f, stream, len, 0, emit, &sink);
    mav_framer_flush(&f, emit, &sink);

    TEST_ASSERT_EQUAL_UINT(len, sink.total);
    TEST_ASSERT_EQUAL_MEMORY(stream, sink.data, len);
    for (size_t k = 0; k < sink.n; k++) {
        TEST_ASSERT_TRUE(sink.frame_len[k] <= MAV_FRAMER_MAX_OUT);
        const size_t end = sink.frame_off[k] + sink.frame_len[k];
        bool on_boundary = false;
        for (size_t j = 0; j < np; j++) {
            on_boundary |= (pkt_ends[j] == end);
        }
        TEST_ASSERT_TRUE_MESSAGE(on_boundary, "frame ended mid-packet");
    }
    TEST_ASSERT_EQUAL_UINT32(20, f.packets);
}

/* REQ-SER-03: raw (non-MAVLink) bytes and partial packets are forwarded. */
static void test_raw_and_partial(void)
{
    mav_framer_t f;
    mav_framer_init(&f, 300);
    const uint8_t text[] = "hello\r\n";
    uint8_t p[64];
    size_t n = make_v1(p, 10, 0x42);

    mav_framer_push(&f, text, sizeof(text) - 1u, 0, emit, &sink);
    mav_framer_push(&f, p, n, 0, emit, &sink);
    mav_framer_push(&f, p, 5, 0, emit, &sink); /* truncated packet */
    mav_framer_flush(&f, emit, &sink);

    TEST_ASSERT_EQUAL_UINT(sizeof(text) - 1u + n + 5u, sink.total);
    TEST_ASSERT_EQUAL_MEMORY(text, sink.data, sizeof(text) - 1u);
    TEST_ASSERT_EQUAL_MEMORY(p, &sink.data[sizeof(text) - 1u], n);
    TEST_ASSERT_TRUE(mav_scan_at_boundary(&f.scan));

    mav_framer_push(&f, p, 3, 0, emit, &sink);
    mav_framer_discard(&f);
    mav_framer_flush(&f, emit, &sink);
    TEST_ASSERT_EQUAL_UINT(sizeof(text) - 1u + n + 5u, sink.total);
}

/* Property test: any byte stream in any chunking comes out identical and in
 * frames no larger than the configured capacity. */
static void test_transparency_fuzz(void)
{
    srand(12345);
    static uint8_t in[40000];
    for (int round = 0; round < 200; round++) {
        memset(&sink, 0, sizeof(sink));
        mav_framer_t f;
        mav_framer_init(&f, (uint16_t)(MAV_MAX_PACKET + (rand() % 11)));
        size_t len = (size_t)(rand() % (int)sizeof(in));
        for (size_t i = 0; i < len; i++) {
            const int r = rand() % 100;
            in[i] = r < 3 ? MAV_STX_V2 : r < 6 ? MAV_STX_V1 : (uint8_t)rand();
        }
        size_t pos = 0;
        uint32_t now = 0;
        while (pos < len) {
            size_t chunk = 1u + (size_t)(rand() % 300);
            if (chunk > len - pos) {
                chunk = len - pos;
            }
            mav_framer_push(&f, &in[pos], chunk, now, emit, &sink);
            pos += chunk;
            now += (uint32_t)(rand() % 4);
            if (rand() % 5 == 0) {
                mav_framer_poll(&f, now, 5, emit, &sink);
            }
            if (rand() % 17 == 0) {
                mav_framer_flush(&f, emit, &sink);
            }
        }
        mav_framer_flush(&f, emit, &sink);
        TEST_ASSERT_EQUAL_UINT(len, sink.total);
        if (len > 0u) {
            TEST_ASSERT_EQUAL_MEMORY(in, sink.data, len);
        }
        for (size_t k = 0; k < sink.n; k++) {
            TEST_ASSERT_TRUE(sink.frame_len[k] > 0u && sink.frame_len[k] <= f.out_cap);
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_scan_lengths);
    RUN_TEST(test_poll_hold_time);
    RUN_TEST(test_no_split);
    RUN_TEST(test_raw_and_partial);
    RUN_TEST(test_transparency_fuzz);
    return UNITY_END();
}
