# Tower relay: ESP32 ground units on a Raspberry Pi

A relay needs **no special firmware mode**. The tower runs ordinary GROUND units plugged into a
Raspberry Pi (or any Linux box) over USB, and a MAVLink router forwards the traffic to the
servers inside the building.

```
 drone 1 ─ AIR ◄─ch 13─► GROUND 1 ─USB─┐
 drone 2 ─ AIR ◄─ch  8─► GROUND 2 ─USB─┤  Raspberry Pi ── mavlink-routerd ──► GCS / servers
 drone 3 ─ AIR ◄─ch  3─► GROUND 3 ─USB─┘   (tower)         (or MAVProxy)       (UDP / TCP)
```

## Is one ESP32 enough?

**For one drone, yes.** One pair carries a normal telemetry link: 57600 baud is about 46 kbit/s,
while LR 250 kbps has several times that on air.

**For several drones, you need one ground unit per drone.** The limit is pairing, not bandwidth.
A ground unit pairs with exactly one air unit, which is what makes the authentication and
failover simple and predictable. That's why a multi-drone tower is several independent pairs,
not one radio talking to many.

**Sharing a channel.** Pairs on the same channel share airtime. These are **estimates, not
measurements**, and should be checked with the `lost` / `rxerr` counters:

* A drone streaming at 4 Hz, plus link heartbeats, uses roughly 15–25 % airtime at LR 250 kbps,
  so **2–3 drones per channel** is a comfortable limit.
* Beyond that, give each pair its own channel: `PAIR=drone2 scripts/set_channel.sh 8`.
* When ground units sit next to each other in the tower, keep their channels **at least 5 apart**
  (for example 13 / 8 / 3), and space their antennas about 1 m apart. At 20 dBm a neighbouring
  transmitter can desensitise a receiver even on a different channel.

**Not included:** one drone talking to *two* towers for coverage or redundancy. That would need
multi-peer firmware. It's a deliberate non-goal for now.

## 1. Build one pair per drone

```bash
PAIR=drone1 scripts/gen_key.sh
```
```bash
PAIR=drone1 scripts/set_channel.sh 13
```
```bash
PAIR=drone1 scripts/build.sh air esp32s3 -p /dev/cu.usbmodemXXXX flash
```
```bash
PAIR=drone1 scripts/build.sh ground esp32s3 -p /dev/cu.usbmodemYYYY flash
```

Repeat with `PAIR=drone2` and a different channel, and so on. Each `pairs/<name>.defaults` file
holds that pair's secret; it's git-ignored, so back it up.

Give every drone a unique `SYSID_THISMAV` (1, 2, 3, ...), so the router and the GCS can tell the
drones apart.

## 2. Stable device names on the Pi (udev)

An ESP32-C3/S3 on its built-in USB reports its MAC address as the USB serial number, so each ground
unit can get a fixed name whatever order they enumerate in. Create
`/etc/udev/rules.d/90-mav-relay.rules`:

```
# ESP32-S3/C3 USB-Serial-JTAG (303a:1001), matched by MAC. One line per ground unit.
SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", ATTRS{idProduct}=="1001", ATTRS{serial}=="EC:DA:3B:62:5D:CC", SYMLINK+="mav-drone1", TAG+="systemd", ENV{ID_MM_DEVICE_IGNORE}="1"
SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", ATTRS{idProduct}=="1001", ATTRS{serial}=="AA:BB:CC:DD:EE:FF", SYMLINK+="mav-drone2", TAG+="systemd", ENV{ID_MM_DEVICE_IGNORE}="1"
```

* `TAG+="systemd"` creates a `dev-mav\x2ddrone1.device` unit, so the MAVProxy service below can follow the device.
* `ID_MM_DEVICE_IGNORE` stops ModemManager from probing the port with AT commands, which it
  otherwise does to every new `ttyACM`, and those commands would be injected into the MAVLink
  stream. Alternatively, remove ModemManager: `sudo apt purge modemmanager`.
* Read a unit's MAC with `udevadm info -a -n /dev/ttyACM0 | grep serial`.
* Classic ESP32 boards with CP210x/CH340 chips often share one serial number. For those, match
  the physical USB port with `KERNELS=="1-1.2"` instead.

Apply the rules:

```bash
sudo udevadm control --reload && sudo udevadm trigger
```

## 3. Routing to the servers

### Recommended: mavlink-router (one process for every drone)

mavlink-router is a plain router. It **does not send heartbeats of its own**, so a drone's GCS
failsafe (`FS_GCS_ENABLE`) still fires if either the radio link *or* the operator's GCS in the
building goes away. `/etc/mavlink-router/main.conf`:

```ini
# GCSs inside the building can also connect in by TCP on 5760.
[General]
TcpServerPort = 5760

# Baud is ignored on native USB but required by the parser.
[UartEndpoint drone1]
Device = /dev/mav-drone1
Baud = 57600

[UartEndpoint drone2]
Device = /dev/mav-drone2
Baud = 57600

# Mode Normal = push to this server.
[UdpEndpoint servers]
Mode = Normal
Address = 10.0.0.10
Port = 14550
```

Then run it:

```bash
sudo systemctl enable --now mavlink-router
```

Messages are routed by system id, so unique `SYSID_THISMAV` values matter.

### Alternative: one MAVProxy per drone

This works fine if you already use MAVProxy. **Turn off MAVProxy's own heartbeat**
(`set heartbeat 0`). Otherwise MAVProxy looks like a live GCS to the drone, and the drone would
never fail safe if the building's GCS or network went down, as long as the radio link held.

`/etc/systemd/system/mavproxy@.service`:

```ini
[Unit]
Description=MAVProxy relay for %i
BindsTo=dev-mav\x2d%i.device
After=dev-mav\x2d%i.device network-online.target

[Service]
EnvironmentFile=/etc/mavproxy/%i.env
ExecStart=/usr/local/bin/mavproxy.py --master=/dev/mav-%i --baudrate=57600 \
    --non-interactive --state-basedir=/var/lib/mavproxy/%i \
    "--cmd=set heartbeat 0" $MAVPROXY_OUTS
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
```

`/etc/mavproxy/drone1.env`:

```
MAVPROXY_OUTS=--out=udpout:10.0.0.10:14550 --out=tcpin:0.0.0.0:5761
```

Then start it:

```bash
sudo systemctl enable --now mavproxy@drone1
```

`Restart=always` plus `BindsTo` the device means that unplugging and replugging a ground unit
restarts only that drone's relay.

## 4. Power and placement

* An ESP32-S3 transmitting at 20 dBm draws current peaks of a few hundred mA. A Raspberry Pi 4
  gives all its USB ports 1.2 A in total, so **use a powered USB hub for more than two ground
  units**. Brown-outs reset the ESP32 (it recovers by itself, but you lose link for about 1 s).
* Mount the antennas at the top of the tower, not the Pi. Keep USB cables short, or use
  USB-over-Cat5 extenders, instead of long RF coax at 2.4 GHz.

## 5. Checks before relying on it

1. Each `/dev/mav-*` exists. The per-drone link test passes on the Pi before the router starts:
   ```bash
   tools/link_test.py --port /dev/mav-drone1 --duration 600
   ```
2. **DTR/RTS on open:** opening the port on macOS didn't reset the S3 during development. Confirm
   the same on the Pi by starting the router and checking that the unit doesn't reboot. The air
   unit's log, or a missing re-pair, shows this.
3. From a server in the building, the GCS sees every drone under its own system id.
4. Stop the router: each drone's GCS failsafe triggers after `FS_GCS_TIMEOUT`.
