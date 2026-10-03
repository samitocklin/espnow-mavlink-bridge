/*
 * bridge - the two data-path tasks between the serial port and the radio.
 *   serial_rx: serial -> MAVLink-aware framer -> radio TX queue
 *   serial_tx: radio RX -> serial, plus RADIO_STATUS injection and stats
 */
#pragma once

#include <stdbool.h>

void bridge_start(bool radio_status_enabled);
