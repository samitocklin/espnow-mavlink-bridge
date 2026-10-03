/*
 * serial_port - the MAVLink-carrying serial port (UART or built-in USB),
 * selected in menuconfig. Single reader task and single writer task.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sport_init(void);

/* Returns bytes read (0 on timeout). Returns as soon as any data is
 * available, draining what is already buffered up to `cap`. */
size_t sport_read(uint8_t *buf, size_t cap, uint32_t timeout_ms);

/* All-or-nothing, never blocks for long. Returns false (and counts a drop) if
 * the whole buffer cannot be queued, so a MAVLink packet is never cut.
 * USB: output older than 1 s that the host has not read is discarded. */
bool sport_write(const uint8_t *buf, size_t len);

/* Must be called often by the writer task (every loop) to move queued output
 * to the hardware and expire stale output. */
void sport_service(void);

/* True while output is queued; the writer task should then poll fast. */
bool sport_tx_pending(void);

const char *sport_name(void);

#ifdef __cplusplus
}
#endif
