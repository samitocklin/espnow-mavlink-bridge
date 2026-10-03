#include "bridge.h"

#include <stdatomic.h>

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "health.h"
#include "mav_framer.h"
#include "radio.h"
#include "radio_status.h"
#include "sdkconfig.h"
#include "serial_port.h"

static const char *TAG = "bridge";

#define SERIAL_IDLE_MS         2u    /* line idle -> flush everything */
#define OUT_STALE_MS           50u   /* unfinished outgoing packet abandoned */
#define RADIO_STATUS_PERIOD_MS 1000u
#define STATS_PERIOD_MS        10000u
#define TASK_STACK             4096u
#define SERIAL_RX_PRIO         19
#define SERIAL_TX_PRIO         18

static bool         s_radio_status;
static atomic_uint  s_local_mav_version; /* spoken by the device on our serial port */

static StaticTask_t s_rx_tcb, s_tx_tcb;
static StackType_t  s_rx_stack[TASK_STACK], s_tx_stack[TASK_STACK];

static inline bool due(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

/* ---- serial -> air ----------------------------------------------------------- */

static void emit_to_radio(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    (void)radio_submit(data, len); /* drops are counted inside */
}

static void serial_rx_task(void *arg)
{
    (void)arg;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    static mav_framer_t framer;
    static uint8_t      buf[256];
    mav_framer_init(&framer, MAV_FRAMER_MAX_OUT);
    uint32_t epoch = radio_link_epoch();

    for (;;) {
        (void)esp_task_wdt_reset();
        const size_t   n   = sport_read(buf, sizeof(buf), SERIAL_IDLE_MS);
        const uint32_t now = health_uptime_ms();

        const uint32_t e = radio_link_epoch();
        if (e != epoch) {
            epoch = e;
            mav_framer_discard(&framer); /* never forward pre-outage data */
        }

        if (n > 0u) {
            mav_framer_push(&framer, buf, n, now, emit_to_radio, NULL);
            mav_framer_poll(&framer, now, CONFIG_BRIDGE_MAX_HOLD_MS, emit_to_radio, NULL);
        } else {
            mav_framer_flush(&framer, emit_to_radio, NULL);
        }
        if (framer.last_version != 0u) {
            atomic_store(&s_local_mav_version, framer.last_version);
        }
    }
}

/* ---- air -> serial ------------------------------------------------------------ */

static void inject_radio_status(uint8_t *seq)
{
    radio_snapshot_t s;
    radio_snapshot(&s);
    const bool up = s.state != LINK_SEARCHING;

    const uint32_t errs = s.rx_lost + health_fault_count(FLT_AIR_RX_BAD);
    radio_status_t rs = {
        .rssi     = up ? radio_status_dbm_to_sik(s.rssi) : 0u,
        .remrssi  = up ? radio_status_dbm_to_sik(s.rem_rssi) : 0u,
        .txbuf    = s.txbuf_pct,
        .noise    = up ? radio_status_dbm_to_sik(s.noise) : 0u,
        .remnoise = up ? radio_status_dbm_to_sik(s.rem_noise) : 0u,
        .rxerrors = errs > 0xFFFFu ? 0xFFFFu : (uint16_t)errs,
        .fixed    = 0u,
    };
    const unsigned v = atomic_load(&s_local_mav_version);
    uint8_t out[RADIO_STATUS_MAX_BYTES];
    const size_t n = radio_status_encode(&rs, v == 2u ? 2u : 1u, (*seq)++, out, sizeof(out));
    if (n > 0u) {
        (void)sport_write(out, n);
    }
}

static void log_stats(void)
{
    radio_snapshot_t s;
    radio_snapshot(&s);
    ESP_LOGI(TAG, "%s pwr %d dBm rssi %d/%d dBm rx %u tx %u lost %u dup %u losses %u txbuf %u%% "
                  "drops: nolink %u qfull %u ser_tx %u ser_stale %u ser_ovf %u auth %u",
             link_state_name(s.state), s.tx_power_qdbm / 4, s.rssi, s.rem_rssi, (unsigned)s.rx_data,
             (unsigned)s.tx_data, (unsigned)s.rx_lost, (unsigned)s.rx_dup, (unsigned)s.losses,
             s.txbuf_pct, (unsigned)health_fault_count(FLT_AIR_TX_NOLINK),
             (unsigned)health_fault_count(FLT_AIR_TX_QFULL),
             (unsigned)health_fault_count(FLT_SERIAL_TX_DROP),
             (unsigned)health_fault_count(FLT_SERIAL_TX_STALE),
             (unsigned)health_fault_count(FLT_SERIAL_RX_OVERFLOW),
             (unsigned)health_fault_count(FLT_AUTH_FAIL));
}

static void serial_tx_task(void *arg)
{
    (void)arg;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    static radio_rx_t rx;
    mav_scan_t scan;
    mav_scan_reset(&scan);
    uint8_t  rs_seq     = 0;
    uint32_t last_write = 0;
    uint32_t next_rs    = health_uptime_ms() + RADIO_STATUS_PERIOD_MS;
    uint32_t next_stats = health_uptime_ms() + STATS_PERIOD_MS;
    uint32_t epoch      = radio_link_epoch();

    for (;;) {
        (void)esp_task_wdt_reset();
        /* Poll fast while output is queued for the host, otherwise idle-wait. */
        if (radio_rx_wait(&rx, sport_tx_pending() ? 1u : 20u)) {
            const uint8_t *p = NULL;
            const size_t   n = radio_rx_process(&rx, &p);
            if (n > 0u && sport_write(p, n)) {
                for (size_t i = 0; i < n; i++) {
                    (void)mav_scan_byte(&scan, p[i]);
                }
                last_write = health_uptime_ms();
            }
        }
        sport_service();
        const uint32_t now = health_uptime_ms();

        const uint32_t e = radio_link_epoch();
        if (e != epoch) {
            epoch = e;
            mav_scan_reset(&scan);
        }
        if (!mav_scan_at_boundary(&scan) && (uint32_t)(now - last_write) >= OUT_STALE_MS) {
            /* The rest of that packet was lost over the air; the endpoint's
             * parser will discard it, so it is safe to write again. */
            mav_scan_reset(&scan);
        }
        if (s_radio_status && due(now, next_rs) && mav_scan_at_boundary(&scan)) {
            inject_radio_status(&rs_seq);
            next_rs = now + RADIO_STATUS_PERIOD_MS;
        }
        if (due(now, next_stats)) {
            log_stats();
            next_stats = now + STATS_PERIOD_MS;
        }
    }
}

void bridge_start(bool radio_status_enabled)
{
    s_radio_status = radio_status_enabled;
    TaskHandle_t a = xTaskCreateStatic(serial_rx_task, "ser_rx", TASK_STACK, NULL,
                                       SERIAL_RX_PRIO, s_rx_stack, &s_rx_tcb);
    TaskHandle_t b = xTaskCreateStatic(serial_tx_task, "ser_tx", TASK_STACK, NULL,
                                       SERIAL_TX_PRIO, s_tx_stack, &s_tx_tcb);
    if (a == NULL || b == NULL) {
        health_fatal(FLT_INIT, "bridge tasks");
    }
}
