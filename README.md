# espnow-mavlink-bridge

A pair of ESP32s that works like a SiK/RFD900 telemetry radio pair, over ESP-NOW.

```
 GCS (Mission Planner / QGC / WebGCS)                          ArduPilot FC
        │ USB serial                                        TELEM UART │
   ┌────┴─────┐        ESP-NOW (encrypted + authenticated)   ┌─────────┴┐
   │  GROUND  │ ◄──────────────────────────────────────────► │   AIR    │
   └──────────┘                                              └──────────┘
```

* **GROUND unit:** plugs into the computer. The GCS just sees a serial port carrying MAVLink.
* **AIR unit:** wired to a flight-controller telemetry UART.
* **Byte-transparent:** anything that isn't MAVLink is forwarded too. The bridge is MAVLink-*aware*: it never splits a packet across radio frames, so a lost frame costs whole packets instead of corrupting the ones around it.
* **Link quality:** both ends inject **RADIO_STATUS** at 1 Hz, exactly like a SiK radio. The GCS shows RSSI, and ArduPilot uses `txbuf` to slow its telemetry down when the link is congested.
* **Automatic pairing** from a shared key. You never type MAC addresses.
* **Supported chips:** ESP32, ESP32-C3 and ESP32-S3, in either role.

> **Safety notice.** This project follows safety-critical embedded *practices*: static memory,
> watchdogs, fault accounting, requirements traced to tests (see
> [docs/REQUIREMENTS.md](docs/REQUIREMENTS.md)). It is **not certified** to DO-178C or any
> airworthiness standard. ESP-NOW on 2.4 GHz has limited range and is easy to jam. Always
> configure the autopilot's own failsafes, and keep an independent RC link for manual control.

## Quick start

Requires ESP-IDF v5.5 (`~/esp/esp-idf`, or set `IDF_PATH`).

```bash
scripts/gen_key.sh
```
This creates `sdkconfig.defaults.local` with a random 128-bit key and network id. The file is git-ignored. Build **both** units from this checkout so they share the key.

```bash
scripts/build.sh ground esp32s3 -p /dev/cu.usbmodem1101 flash
```
```bash
scripts/build.sh air esp32c3 -p /dev/cu.usbmodem2101 flash
```

Use your own chip type and port for each unit.

### Changing the radio channel

```bash
scripts/set_channel.sh 9 US
```

This picks the channel for the pair, plus the country code if you give one. Both are saved next to the key in `sdkconfig.defaults.local`, so both units always match. Rebuild and reflash **both** units afterwards.

The default is channel 13, which is legal in the EU and most countries. In the US and Canada, use 1–11. Channels 3 or 9 sit between the common Wi-Fi channels 1, 6 and 11. `scripts/build.sh <role> <target>` on its own just builds; any extra arguments are passed to `idf.py` (`flash`, `monitor`, `menuconfig`, ...).

Power both units. Within about half a second the LEDs go solid, and the GCS can connect to the ground unit's port.

### Wiring the air unit

| ESP32 pin (default) | ESP32 | ESP32-C3 | ESP32-S3 | Flight controller TELEM |
|---|---|---|---|---|
| UART TX | GPIO17 (UART1) | GPIO5 (UART1) | GPIO43 (UART0, "TX") | RX |
| UART RX | GPIO16 (UART1) | GPIO4 (UART1) | GPIO44 (UART0, "RX") | TX |
| GND | GND | GND | GND | GND |
| 5V / VIN | 5V | 5V | 5V | 5V (check the port's current budget, ~250 mA peak) |

The logic level is 3.3 V, which suits Pixhawk-class TELEM ports. On the ESP32-S3 the air unit uses UART0, the pins labelled TX/RX on most dev boards, so its logs go to the built-in USB port. The ROM prints a few boot lines on UART0 at power-up; ArduPilot ignores them. You can change the pins, baud rate and RTS/CTS flow control in `idf.py menuconfig` → *ESP-NOW MAVLink bridge → Serial port*.

### Ground unit port

| Chip | GCS sees | Logs go to |
|---|---|---|
| ESP32-C3 / S3 | the built-in USB port (`/dev/cu.usbmodem*`, `COMx`). Baud rate is ignored. | UART0 pins (TX GPIO21 on C3, GPIO43 on S3) |
| Classic ESP32 | the board's USB-UART chip, at **57600** baud | nowhere: the console is disabled, because UART0 carries MAVLink |

The build refuses to compile any configuration that would put logs on the MAVLink port.

On the built-in USB port, the unit can't tell whether a program has the port open. Output is therefore kept in a timestamped queue, and anything the host hasn't read within **1 s** is discarded. A GCS that connects later gets current telemetry, not a backlog.

## ArduPilot setup

| Parameter | Value | Why |
|---|---|---|
| `SERIALx_PROTOCOL` | `2` (MAVLink2) | |
| `SERIALx_BAUD` | `57` (57600) | Must match `BRIDGE_UART_BAUD`. 115200 works at 1 Mbps. |
| `FS_GCS_ENABLE` | `1` (RTL) or `2`+ | **Required.** The bridge never fakes GCS heartbeats, so a lost link is seen as GCS loss. |
| `FS_GCS_TIMEOUT` | `5` | Seconds of GCS silence before the failsafe triggers. |
| `SRx_*` stream rates | 2–10 Hz | Leaves headroom. The link also throttles through RADIO_STATUS. |

## Behaviour

### Link states

| State | Meaning | LED |
|---|---|---|
| SEARCHING | Broadcasting authenticated pairing beacons | 1 Hz blink (double-blink in safe mode) |
| CONNECTED | Heartbeats from the peer within 300 ms | solid |
| DEGRADED | Peer silent for 300–1500 ms. Data still flows | fast blink (~4 Hz) |
| (LOST) | Peer silent for 1500 ms. Queued data in both directions is **discarded**, so nothing is delivered late, and the unit goes back to SEARCHING | – |
| CONFIG FAULT | Missing or invalid key. **The radio stays off** | 10 Hz blink |

If a peer restarts, this is detected as soon as it starts searching again, and the link re-forms without anyone touching it.

### Failure handling

| Fault | Response |
|---|---|
| Task hang | Task watchdog (2 s) → panic → reboot |
| Wi-Fi / ESP-NOW / UART init failure | Retried 3× with backoff, then a controlled restart |
| 5 abnormal resets in a row | Safe mode: plain bridge, RADIO_STATUS off, LED double-blink. Cleared after 60 s stable or a power cycle |
| Brown-out | Hardware brown-out reset. Counts toward the boot-loop guard |
| Serial RX overflow / framing error | Counted, the driver is resynchronised, reported in RADIO_STATUS `rxerrors` |
| Air TX queue full | Oldest frame dropped (counted). `txbuf` falls, so ArduPilot slows down |
| USB host not reading (port closed) | Output older than 1 s discarded (`ser_stale` counter), so there's no stale backlog on connect |
| Corrupt, foreign, replayed or forged frames | Rejected and counted, never forwarded |

Every 10 s each unit logs its state, RSSI, frame counters and drop counters.

### Security model

* **Key derivation:** from the shared key and network id, HMAC-SHA256 derives separate keys for pairing beacons, per-frame authentication and ESP-NOW encryption (PMK/LMK).
* **Pairing:** each unit accepts a peer only if the peer's authenticated beacon echoes this unit's current random nonce. A recorded beacon therefore can't be replayed to pair.
* **Per-frame authentication:** every unicast frame carries a 128-bit HMAC tag bound to the session (both nonces). It's checked before the sequence window, so forged, cross-session or replayed frames never reach the flight controller. ESP-NOW's CCMP encryption provides confidentiality. Authenticity doesn't depend on it, because ESP-NOW doesn't document rejecting unencrypted unicast frames.
* **Limitations:** this doesn't protect against jamming. An attacker who can replay a peer's old beacon while the link is *already* degraded can force an early re-pair, which is no worse than jamming.

### Getting the most range

The firmware already uses the settings that matter most for range: LR 250 kbps, 20 dBm, a 20 MHz channel on 13 away from Wi-Fi, and PHY power-save off. Beyond that, range comes from hardware and setup:

* **Antennas:** use ESP32-S3 boards with a U.FL connector and external antenna (for example ESP32-S3-DevKitC-1U) instead of PCB antennas. A 5 dBi omni on the ground side typically adds as much range as everything else combined. On the drone, mount the antenna vertically, away from carbon fibre, motors and ESC wiring. (Keep total EIRP within your local limit.)
* **Keep telemetry light:** at LR 250 kbps, use `SERIALx_BAUD` 57600 and modest `SRx_*` stream rates (2–4 Hz). Shorter bursts mean fewer frames lost at the edge of range.
* **Line of sight:** 2.4 GHz is blocked by bodies, terrain and foliage. Raising the ground antenna by 2 m helps more than any setting.

## Configuration (`idf.py menuconfig`)

| Setting | Default |
|---|---|
| Wi-Fi channel | **13** (2472 MHz): the furthest from the usual Wi-Fi channels 1, 6 and 11. Change it with `scripts/set_channel.sh` |
| Country code | **FI**. Sets which channels and power levels are legal. If the channel isn't allowed there, the unit refuses to start the radio (LED 10 Hz) |
| TX power | **20 dBm**: the ESP32 hardware maximum and the EU legal EIRP limit. The applied value is logged at boot |
| Air data rate | **Long Range 250 kbps**: the most range (~8–10 dB more link budget than 1 Mbps). It carries 57600-baud telemetry with room to spare. Use 1 Mbps for serial speeds of 230400 baud or more at short range |
| Heartbeat / degraded / lost | 100 / 300 / 1500 ms |
| Max batching hold | 5 ms |
| RADIO_STATUS injection | on |
| Status LED | GPIO2 (ESP32), GPIO8 active-low (C3), none (S3) |

## Tower relay (several drones, Raspberry Pi)

A relay doesn't need a special firmware mode. Plug one GROUND unit per drone into a Raspberry Pi
and route everything with mavlink-router or MAVProxy. Each drone pair gets its own key and channel:

```bash
PAIR=drone2 scripts/gen_key.sh
```

That's all the tooling there is. See [docs/RELAY.md](docs/RELAY.md) for capacity, channel
planning, udev rules and service files.

## Development

```bash
scripts/test_host.sh
```
Runs the host unit tests (Unity, with ASan + UBSan).

```bash
scripts/build_all.sh
```
Builds the compile matrix (3 chips × 2 roles, `-Werror`).

```bash
tools/link_test.py --port /dev/cu.usbmodem1101 --duration 600
```
Runs the end-to-end test through the ground unit. It needs `pymavlink`.

### Code layout

```
main/                 app_main (start-up order), radio (ESP-NOW, pairing, link supervision), bridge (data-path tasks)
components/
  link_proto/         air frame format, CRC, sequence window, link state machine   (pure C, host-tested)
  mav_framer/         MAVLink boundary detection, frame packing, RADIO_STATUS      (pure C, host-tested)
  pairing/            key derivation, beacon + per-frame HMAC                      (pure C + mbedtls, host-tested)
  serial_port/        UART / USB-Serial-JTAG behind one all-or-nothing API
  health/             fault counters, boot-loop guard, safe mode, watchdog, LED
test/host/            unit tests
docs/REQUIREMENTS.md  requirement → verification trace and hardware test procedures
```

## License

MIT
