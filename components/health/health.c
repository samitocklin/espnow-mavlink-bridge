#include "health.h"

#include <stdatomic.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "health";

#define BOOTLOOP_MAGIC        0x424C4F4Fu /* "BLOO" */
#define BOOTLOOP_LIMIT        5u
#define BOOTLOOP_CLEAR_MS     60000u
#define LED_STEP_MS           50u
#define LED_STEPS             20u        /* 1 s pattern */
#define FAULT_LOG_INTERVAL_MS 5000u

/* Survives software resets and watchdog/panic resets, not power loss. */
static RTC_NOINIT_ATTR uint32_t s_rtc_magic;
static RTC_NOINIT_ATTR uint32_t s_rtc_bad_resets;
static RTC_NOINIT_ATTR uint32_t s_rtc_fatal_pending;

static bool                s_safe_mode;
static atomic_uint_fast32_t s_faults[FLT_COUNT];
static uint32_t            s_fault_logged_ms[FLT_COUNT];
static atomic_int          s_led_mode = LED_OFF;

static const char *const FAULT_NAMES[FLT_COUNT] = {
    [FLT_SERIAL_RX_OVERFLOW] = "serial_rx_overflow",
    [FLT_SERIAL_RX_FRAMING]  = "serial_rx_framing",
    [FLT_SERIAL_TX_DROP]     = "serial_tx_drop",
    [FLT_SERIAL_TX_STALE]    = "serial_tx_stale",
    [FLT_AIR_TX_FAIL]        = "air_tx_fail",
    [FLT_AIR_TX_TIMEOUT]     = "air_tx_timeout",
    [FLT_AIR_TX_ERR]         = "air_tx_err",
    [FLT_AIR_TX_QFULL]       = "air_tx_qfull",
    [FLT_AIR_TX_NOLINK]      = "air_tx_nolink",
    [FLT_AIR_RX_QFULL]       = "air_rx_qfull",
    [FLT_AIR_RX_BAD]         = "air_rx_bad",
    [FLT_AUTH_FAIL]          = "auth_fail",
    [FLT_PEER_MISMATCH]      = "peer_mismatch",
    [FLT_LINK_LOST]          = "link_lost",
    [FLT_CONFIG]             = "config",
    [FLT_INIT]               = "init",
};

/* One bit per 50 ms step, LSB first. */
static const uint32_t LED_PATTERNS[] = {
    [LED_OFF]          = 0x00000u,
    [LED_SEARCHING]    = 0x003FFu, /* 1 Hz, 50 % */
    [LED_CONNECTED]    = 0xFFFFFu, /* solid */
    [LED_DEGRADED]     = 0x39CE7u, /* ~4 Hz */
    [LED_CONFIG_FAULT] = 0x55555u, /* 10 Hz */
};
static const uint32_t LED_SAFE_SEARCHING = 0x00033u; /* double blink */

uint32_t health_uptime_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool reset_is_abnormal(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
    case ESP_RST_UNKNOWN:
        return true;
    default:
        return false;
    }
}

void health_init(void)
{
    const esp_reset_reason_t reason = esp_reset_reason();
    if (reason == ESP_RST_POWERON || s_rtc_magic != BOOTLOOP_MAGIC) {
        s_rtc_magic         = BOOTLOOP_MAGIC;
        s_rtc_bad_resets    = 0;
        s_rtc_fatal_pending = 0;
    }
    const bool abnormal = reset_is_abnormal(reason) ||
                          (reason == ESP_RST_SW && s_rtc_fatal_pending != 0u);
    s_rtc_fatal_pending = 0;
    if (abnormal) {
        s_rtc_bad_resets++;
    } else {
        s_rtc_bad_resets = 0;
    }
    s_safe_mode = s_rtc_bad_resets >= BOOTLOOP_LIMIT;

    ESP_LOGI(TAG, "reset reason %d, consecutive abnormal resets %u%s", (int)reason,
             (unsigned)s_rtc_bad_resets, s_safe_mode ? " -> SAFE MODE" : "");

#if CONFIG_BRIDGE_LED_GPIO >= 0
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_BRIDGE_LED_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
    };
    if (gpio_config(&io) != ESP_OK) {
        ESP_LOGW(TAG, "LED GPIO %d unavailable", CONFIG_BRIDGE_LED_GPIO);
    }
#endif
}

bool health_safe_mode(void)
{
    return s_safe_mode;
}

void health_fault(fault_id_t id)
{
    if ((unsigned)id >= FLT_COUNT) {
        return;
    }
    const uint32_t n   = (uint32_t)atomic_fetch_add(&s_faults[id], 1u) + 1u;
    const uint32_t now = health_uptime_ms();
    /* Rate-limited so a fault storm cannot starve the CPU with logging. */
    if (n == 1u || (uint32_t)(now - s_fault_logged_ms[id]) >= FAULT_LOG_INTERVAL_MS) {
        s_fault_logged_ms[id] = now;
        ESP_LOGW(TAG, "fault %s (count %u)", FAULT_NAMES[id], (unsigned)n);
    }
}

uint32_t health_fault_count(fault_id_t id)
{
    return (unsigned)id < FLT_COUNT ? (uint32_t)atomic_load(&s_faults[id]) : 0u;
}

const char *health_fault_name(fault_id_t id)
{
    return (unsigned)id < FLT_COUNT ? FAULT_NAMES[id] : "?";
}

void health_fatal(fault_id_t id, const char *why)
{
    health_fault(id);
    ESP_LOGE(TAG, "FATAL %s: %s -> restarting", health_fault_name(id), why ? why : "");
    s_rtc_fatal_pending = 1u;
    vTaskDelay(pdMS_TO_TICKS(100)); /* let the log drain */
    esp_restart();
}

void health_set_led(led_mode_t mode)
{
    atomic_store(&s_led_mode, (int)mode);
}

static void led_write(bool on)
{
#if CONFIG_BRIDGE_LED_GPIO >= 0
#if CONFIG_BRIDGE_LED_ACTIVE_LOW
    on = !on;
#endif
    (void)gpio_set_level(CONFIG_BRIDGE_LED_GPIO, on ? 1u : 0u);
#else
    (void)on;
#endif
}

static StaticTask_t s_task_tcb;
static StackType_t  s_task_stack[2560];

static void health_task(void *arg)
{
    (void)arg;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    uint32_t   step    = 0;
    bool       cleared = false;
    TickType_t wake    = xTaskGetTickCount();

    for (;;) {
        (void)esp_task_wdt_reset();

        const int mode = atomic_load(&s_led_mode);
        uint32_t pattern = ((unsigned)mode < (sizeof(LED_PATTERNS) / sizeof(LED_PATTERNS[0])))
                               ? LED_PATTERNS[mode] : 0u;
        if (s_safe_mode && mode == LED_SEARCHING) {
            pattern = LED_SAFE_SEARCHING;
        }
        led_write(((pattern >> step) & 1u) != 0u);
        step = (step + 1u) % LED_STEPS;

        if (!cleared && health_uptime_ms() >= BOOTLOOP_CLEAR_MS) {
            /* Stable long enough: the next fault reset starts a fresh count. */
            s_rtc_bad_resets = 0;
            cleared = true;
        }
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(LED_STEP_MS));
    }
}

void health_start(void)
{
    TaskHandle_t h = xTaskCreateStatic(health_task, "health",
                                       sizeof(s_task_stack) / sizeof(s_task_stack[0]),
                                       NULL, 2, s_task_stack, &s_task_tcb);
    if (h == NULL) {
        health_fatal(FLT_INIT, "health task");
    }
}
