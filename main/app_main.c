/*
 * ESP-NOW MAVLink bridge - entry point.
 *
 * Start-up order matters: health first (reset classification, LED), then
 * configuration validation (never transmit with an invalid key), serial port,
 * radio, and finally the data-path tasks.
 */
#include "bridge.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "health.h"
#include "pairing.h"
#include "radio.h"
#include "sdkconfig.h"
#include "serial_port.h"

static const char *TAG = "main";

#if defined(CONFIG_BRIDGE_ROLE_AIR)
#define ROLE      PAIR_ROLE_AIR
#define ROLE_NAME "AIR"
#else
#define ROLE      PAIR_ROLE_GROUND
#define ROLE_NAME "GROUND"
#endif

#if defined(CONFIG_BRIDGE_RADIO_STATUS)
#define RADIO_STATUS_ENABLED true
#else
#define RADIO_STATUS_ENABLED false
#endif

_Static_assert(CONFIG_BRIDGE_LOST_MS > CONFIG_BRIDGE_DEGRADED_MS,
               "LOST timeout must exceed DEGRADED timeout");
_Static_assert(CONFIG_BRIDGE_DEGRADED_MS >= 2 * CONFIG_BRIDGE_HEARTBEAT_MS,
               "DEGRADED must allow at least one missed heartbeat");

static void halt_config_fault(const char *why)
{
    health_fault(FLT_CONFIG);
    health_set_led(LED_CONFIG_FAULT);
    health_start();
    /* Deliberately no restart: a bad build would only boot-loop. The radio is
     * never started, so nothing unauthenticated is ever transmitted. */
    for (;;) {
        ESP_LOGE(TAG, "CONFIGURATION FAULT: %s. Radio disabled.", why);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void app_main(void)
{
    health_init();

    uint8_t key[PAIR_KEY_LEN];
    if (!pair_parse_hex_key(CONFIG_BRIDGE_KEY, key)) {
        halt_config_fault("BRIDGE_KEY must be 32 hex characters (run scripts/gen_key.sh)");
    }
    pair_keys_t keys;
    pair_derive_keys(key, (uint16_t)CONFIG_BRIDGE_NET_ID, &keys);

    for (int attempt = 1;; attempt++) {
        if (sport_init() == ESP_OK) {
            break;
        }
        health_fault(FLT_INIT);
        if (attempt >= 3) {
            health_fatal(FLT_INIT, "serial port init");
        }
        vTaskDelay(pdMS_TO_TICKS(100u << attempt));
    }

    const bool safe = health_safe_mode();
    if (!radio_init(ROLE, &keys, (uint16_t)CONFIG_BRIDGE_NET_ID)) {
        halt_config_fault("BRIDGE_WIFI_CHANNEL not allowed by BRIDGE_COUNTRY (see menuconfig)");
    }
    health_start();
    bridge_start(RADIO_STATUS_ENABLED && !safe);
    radio_start();

    ESP_LOGI(TAG, "%s unit running: port %s, channel %d (%s), net 0x%04X%s", ROLE_NAME,
             sport_name(), CONFIG_BRIDGE_WIFI_CHANNEL, CONFIG_BRIDGE_COUNTRY, CONFIG_BRIDGE_NET_ID,
             safe ? ", SAFE MODE (RADIO_STATUS off)" : "");
}
