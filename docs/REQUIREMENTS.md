# Requirements and verification

This is lightweight, DO-178C-*style* traceability: each requirement has an ID,
a rationale, and the means of verification. **It is not a certification
artefact.** The firmware has not been developed or assessed under DO-178C,
DO-254, or any airworthiness standard. Do not use it as the only command link
for an aircraft where losing that link would be hazardous.

Verification methods: **T** = automated test (`scripts/test_host.sh`),
**B** = build-time check (compile matrix / static assertion),
**H** = hardware test procedure (below), **I** = inspection / design review.

## Data path

| ID | Requirement | Method | Evidence |
|---|---|---|---|
| REQ-SER-01 | A well-formed MAVLink v1/v2 (incl. signed) packet shall never be split across two radio frames. | T | `test_mav_framer: test_no_split` |
| REQ-SER-02 | A complete packet shall wait no longer than `BRIDGE_MAX_HOLD_MS` (default 5 ms) for batching. | T | `test_mav_framer: test_poll_hold_time` |
| REQ-SER-03 | The link shall be byte-transparent: every serial byte, MAVLink or not, is delivered once, in order, when the link is up. | T | `test_mav_framer: test_raw_and_partial`, `test_transparency_fuzz` (200 random streams, random chunking) |
| REQ-SER-04 | Serial output shall be all-or-nothing per radio frame; a frame that does not fit is dropped and counted, never truncated (UART). | I | `serial_port.c: sport_write` |
| REQ-SER-05 | No serial write shall block the data path; overload drops the oldest queued data and is counted. | I, H | `radio_submit`, HW-05 |
| REQ-SER-06 | Diagnostic logging shall never share the port that carries MAVLink. | B | `#error` interlocks in `serial_port.c`; negative build verified |
| REQ-SER-07 | On a USB port, output the host has not read within 1 s shall be discarded, so a GCS that opens the port receives current data only. At most one frame may already be inside the USB hardware. | I, H | `serial_port.c` age-limited queue; HW-08 |

## Link

| ID | Requirement | Method | Evidence |
|---|---|---|---|
| REQ-LNK-01 | Frames with a corrupted header, payload, CRC, length or foreign network id shall be rejected. | T | `test_link_proto: test_frame_rejects` (every single-bit flip) |
| REQ-LNK-02 | Duplicate, stale, replayed or out-of-window frames shall be rejected; skipped frames shall be counted as loss. | T | `test_link_proto: test_seq_window`, `test_seq_wrap_and_window` |
| REQ-LNK-03 | The link shall go DEGRADED after `BRIDGE_DEGRADED_MS` (300 ms) and LOST after `BRIDGE_LOST_MS` (1500 ms) of peer silence, and recover on the next valid frame. | T, H | `test_link_proto: test_state_machine*`, HW-02 |
| REQ-LNK-04 | On LOST, all queued data in both directions shall be discarded, so nothing is delivered late after an outage. | I, H | `on_lost_locked`, link epoch in `bridge.c`, HW-02 |
| REQ-LNK-05 | After a peer restart the link shall re-establish without operator action. | H | HW-03 |
| REQ-LNK-06 | The bridge shall not create MAVLink traffic toward the flight controller other than RADIO_STATUS. Link loss therefore appears to the autopilot as loss of GCS heartbeats, and its own GCS failsafe applies. | I | `bridge.c` |

## Security

| ID | Requirement | Method | Evidence |
|---|---|---|---|
| REQ-SEC-01 | Pairing beacons shall be authenticated (HMAC-SHA256, 128-bit tag) over every field and the network id. | T | `test_pairing: test_beacon_auth` |
| REQ-SEC-02 | Every unicast frame shall carry a session-bound HMAC tag; frames from another key or an earlier session shall be rejected. | T | `test_pairing: test_frame_tag_*` |
| REQ-SEC-03 | The firmware shall refuse to transmit with a missing, malformed, placeholder or all-zero key. | T, H | `test_pairing: test_key_parse`, HW-06 |
| REQ-SEC-04 | A peer shall only be accepted if its beacon echoes this unit's current random nonce (liveness; prevents pairing to replayed captures). | I | `handle_beacon_locked` |
| REQ-SEC-06 | The radio shall not start on a channel the configured regulatory country does not allow. This is a configuration fault: the unit halts with the radio off and does not reboot-loop. | H | Verified: channel 13 + `US` halts with repeated CONFIGURATION FAULT, still running at 65 s uptime |
| REQ-SEC-05 | Unicast payloads shall be encrypted (ESP-NOW CCMP, key derived from the shared key). | I | `add_peer_locked` |

## Robustness

| ID | Requirement | Method | Evidence |
|---|---|---|---|
| REQ-ROB-01 | Every task shall be supervised by the task watchdog (2 s); a hang shall reset the unit. | I, B | `esp_task_wdt_add` in every task, `sdkconfig.defaults` |
| REQ-ROB-02 | No heap allocation shall occur in the application after start-up; tasks, queues and buffers are static. | I | `xTaskCreateStatic`, `xQueueCreateStatic` |
| REQ-ROB-03 | Failed Wi-Fi / ESP-NOW / serial init shall be retried with backoff (3 attempts), then a controlled restart. | I | `radio_init`, `app_main` |
| REQ-ROB-04 | After 5 consecutive abnormal resets (panic, WDT, brown-out, fatal) without 60 s of stable operation, the unit shall enter safe mode (plain bridge, RADIO_STATUS off) and show it on the LED. | I | `health.c` |
| REQ-ROB-05 | All fault classes shall be counted and logged with rate limiting. | I | `health_fault` |
| REQ-ROB-06 | The firmware shall compile warning-free (`-Wall -Wextra -Werror -Wshadow`) for ESP32, ESP32-C3 and ESP32-S3 in both roles. | B | `scripts/build_all.sh` |
| REQ-ROB-07 | Host tests shall run clean under AddressSanitizer and UndefinedBehaviorSanitizer. | T | `test/host/CMakeLists.txt` |

## Interface

| ID | Requirement | Method | Evidence |
|---|---|---|---|
| REQ-RS-01 | RADIO_STATUS shall be byte-identical to the reference MAVLink encoder (v1, v2 and v2 truncated). | T | `test_radio_status` (vectors from pymavlink) |
| REQ-RS-02 | Each unit shall inject RADIO_STATUS at 1 Hz, only at a MAVLink packet boundary. | I, H | `bridge.c`, `tools/link_test.py` |
| REQ-SYS-01 | The ground unit shall appear to the GCS as a plain serial port carrying the FC's MAVLink stream. | H | HW-01 |
| REQ-SYS-02 | Packet loss at short range shall be below 2 %. | H | HW-01 (`link_test.py --max-loss 2`) |
| REQ-SYS-03 | Median round-trip latency shall be below 100 ms. | H | HW-01 (`link_test.py --max-rtt 100`) |

## Hardware test procedures

Setup: ground unit on the computer's USB. Air unit's UART connected to an
ArduPilot TELEM port, or to SITL through a USB-UART adapter:
`sim_vehicle.py -v ArduCopter -A "--serial1=uart:/dev/cu.usbserial-XXXX:57600"`.

| ID | Procedure | Pass criteria |
|---|---|---|
| HW-01 | `tools/link_test.py --port <ground port> --duration 600`. Then connect Mission Planner / QGC / WebGCS to the same port. | Script prints PASS. GCS connects, parameters download, link-quality (RSSI) is shown. |
| HW-02 | While connected, power off the air unit for 5 s, then restore it. | Ground log: DEGRADED ≤ 0.35 s, LOST ≤ 1.6 s. The GCS reports link loss; with `FS_GCS_ENABLE` set, ArduPilot's GCS failsafe triggers. The link re-forms ≤ 2 s after power returns. No stale data is delivered after reconnect. |
| HW-03 | Reset (EN button) the air unit, then the ground unit, while telemetry flows. | Link re-forms without operator action each time. |
| HW-04 | Build one unit with a different key (`gen_key.sh --force` on a copy) and power it near the pair. | It never pairs. `auth` / `peer_mismatch` counters increase on the other units. The legitimate pair is unaffected. |
| HW-05 | Set `SERIALx_BAUD` = 921600 on the FC, set the bridge to the same baud with LR 250 kbps, and stream all messages at 50 Hz for 5 min. | `qfull` counter increases, no watchdog reset, RADIO_STATUS `txbuf` falls and ArduPilot throttles its stream rate. |
| HW-06 | Flash a unit built without `sdkconfig.defaults.local`. | LED fast-blinks (10 Hz), the log repeats CONFIGURATION FAULT, and nothing is transmitted. |
| HW-08 | Keep the ground unit's USB port closed for ≥ 25 s while the link runs, then open it and log RADIO_STATUS arrival times. | No burst of backlogged messages. The first message arrives on the normal 1 Hz cadence. |
| HW-07 | 1-hour soak test: `link_test.py --duration 3600`. | PASS, zero unexpected resets (check the `reset reason` line in the log). |
