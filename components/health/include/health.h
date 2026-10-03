/*
 * health - fault accounting, boot-loop guard, watchdog helpers, status LED.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FLT_SERIAL_RX_OVERFLOW = 0,
    FLT_SERIAL_RX_FRAMING,
    FLT_SERIAL_TX_DROP,
    FLT_SERIAL_TX_STALE,   /* output discarded: host not reading for > 1 s */
    FLT_AIR_TX_FAIL,       /* ESP-NOW reported delivery failure */
    FLT_AIR_TX_TIMEOUT,    /* no send callback within the deadline */
    FLT_AIR_TX_ERR,        /* esp_now_send() returned an error */
    FLT_AIR_TX_QFULL,      /* air TX queue full, oldest frame dropped */
    FLT_AIR_TX_NOLINK,     /* data dropped because the link was down */
    FLT_AIR_RX_QFULL,      /* air RX queue full, frame dropped */
    FLT_AIR_RX_BAD,        /* malformed / CRC / wrong network */
    FLT_AUTH_FAIL,         /* beacon failed authentication */
    FLT_PEER_MISMATCH,     /* unicast from a MAC that is not our peer */
    FLT_LINK_LOST,
    FLT_CONFIG,            /* invalid build configuration */
    FLT_INIT,              /* init step failed (retried) */
    FLT_COUNT
} fault_id_t;

typedef enum {
    LED_OFF = 0,
    LED_SEARCHING,
    LED_CONNECTED,
    LED_DEGRADED,
    LED_CONFIG_FAULT,
} led_mode_t;

/* Call first in app_main: classifies the reset reason and updates the
 * boot-loop counter kept in RTC memory. */
void health_init(void);

/* True if too many abnormal resets happened in a row. The application then
 * runs with optional features disabled. */
bool health_safe_mode(void);

void     health_fault(fault_id_t id);
uint32_t health_fault_count(fault_id_t id);
const char *health_fault_name(fault_id_t id);

/* Log and perform a controlled restart. Counts as an abnormal reset. */
void health_fatal(fault_id_t id, const char *why) __attribute__((noreturn));

void health_set_led(led_mode_t mode);

/* Starts the LED / periodic-health task. */
void health_start(void);

uint32_t health_uptime_ms(void);

#ifdef __cplusplus
}
#endif
