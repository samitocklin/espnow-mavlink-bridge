#!/usr/bin/env python3
"""End-to-end link test through the GROUND unit.

Acts as a minimal GCS on the ground unit's serial port and measures what the
flight controller (real or SITL) on the far side of the radio link delivers:

  * FC HEARTBEAT present                      (REQ-SYS-01)
  * RADIO_STATUS injected at ~1 Hz            (REQ-RS-02)
  * MAVLink sequence-gap packet loss          (REQ-SYS-02)
  * round-trip latency via TIMESYNC           (REQ-SYS-03)

Exit status is non-zero if any threshold is violated, so it can gate a soak
test:  tools/link_test.py --port /dev/cu.usbmodem101 --duration 3600
Requires: pip install pymavlink
"""
import argparse
import statistics
import sys
import time

from pymavlink import mavutil


def sik_to_dbm(v):
    return v / 1.9 - 127.0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True, help="ground unit serial port")
    ap.add_argument("--baud", type=int, default=57600, help="ignored for native USB ports")
    ap.add_argument("--duration", type=float, default=60.0, help="seconds")
    ap.add_argument("--max-loss", type=float, default=2.0, help="max FC packet loss %%")
    ap.add_argument("--max-rtt", type=float, default=100.0, help="max median RTT ms")
    ap.add_argument("--stream-rate", type=int, default=4,
                    help="Hz to request for all telemetry streams, like a GCS does (0 = don't request)")
    args = ap.parse_args()

    m = mavutil.mavlink_connection(args.port, baud=args.baud, source_system=255,
                                   source_component=190, autoreconnect=True)
    print(f"listening on {args.port} for {args.duration:.0f} s ...")

    last_seq = {}
    received = {}
    lost = {}
    radio_status = []
    rtts = []
    fc_hb = 0
    disconnects = 0
    t_end = time.monotonic() + args.duration
    next_tx = 0.0

    fc_sys = None
    streams_requested = False
    while time.monotonic() < t_end:
        now = time.monotonic()
        if fc_sys is not None and not streams_requested and args.stream_rate > 0:
            # With SRx_* = 0 the autopilot only sends heartbeats until asked.
            try:
                m.mav.request_data_stream_send(fc_sys, 0, mavutil.mavlink.MAV_DATA_STREAM_ALL,
                                               args.stream_rate, 1)
                streams_requested = True
                print(f"requested all streams at {args.stream_rate} Hz from sys {fc_sys}")
            except (OSError, ConnectionError):
                pass
        if now >= next_tx:
            try:
                m.mav.heartbeat_send(mavutil.mavlink.MAV_TYPE_GCS,
                                     mavutil.mavlink.MAV_AUTOPILOT_INVALID, 0, 0, 0)
                m.mav.timesync_send(0, time.monotonic_ns())
            except (OSError, ConnectionError):
                pass
            next_tx = now + 1.0

        try:
            msg = m.recv_match(blocking=True, timeout=0.2)
        except (OSError, ConnectionError) as e:  # port unplugged / endpoint rebooting
            disconnects += 1
            print(f"link to {args.port} dropped ({e}); retrying")
            time.sleep(1.0)
            continue
        if msg is None or msg.get_type() == "BAD_DATA":
            continue
        src = (msg.get_srcSystem(), msg.get_srcComponent())
        seq = msg.get_seq()
        if src in last_seq:
            gap = (seq - last_seq[src] - 1) & 0xFF
            if gap < 128:  # ignore restarts / reordering
                lost[src] = lost.get(src, 0) + gap
        last_seq[src] = seq
        received[src] = received.get(src, 0) + 1

        t = msg.get_type()
        if t == "HEARTBEAT" and msg.get_srcSystem() != 51 and msg.type != mavutil.mavlink.MAV_TYPE_GCS:
            fc_hb += 1
            fc_sys = msg.get_srcSystem()
        elif t == "RADIO_STATUS":
            radio_status.append(msg)
        elif t == "TIMESYNC" and msg.tc1 != 0:
            rtt_ms = (time.monotonic_ns() - msg.ts1) / 1e6
            if 0 < rtt_ms < 5000:
                rtts.append(rtt_ms)

    ok = True
    print("\n=== results ===")
    print(f"FC heartbeats: {fc_hb}, port disconnects: {disconnects}")
    if fc_hb == 0:
        print("FAIL: no flight-controller HEARTBEAT received")
        ok = False

    for src in sorted(received):
        if src[0] == 51:
            continue
        total = received[src] + lost.get(src, 0)
        loss = 100.0 * lost.get(src, 0) / total if total else 0.0
        print(f"  sys {src[0]:3d} comp {src[1]:3d}: {received[src]:6d} pkts, loss {loss:5.2f} %")
        if loss > args.max_loss:
            print(f"FAIL: loss above {args.max_loss} %")
            ok = False

    rate = len(radio_status) / args.duration
    print(f"RADIO_STATUS: {len(radio_status)} ({rate:.2f} Hz)")
    if radio_status:
        r = radio_status[-1]
        print(f"  last: rssi {sik_to_dbm(r.rssi):.0f} dBm, remrssi {sik_to_dbm(r.remrssi):.0f} dBm, "
              f"txbuf {r.txbuf} %, rxerrors {r.rxerrors}")
    if rate < 0.8:
        print("FAIL: RADIO_STATUS missing or below 0.8 Hz")
        ok = False

    if rtts:
        med = statistics.median(rtts)
        print(f"RTT (TIMESYNC): median {med:.1f} ms, p95 {sorted(rtts)[int(len(rtts) * 0.95) - 1]:.1f} ms, "
              f"n={len(rtts)}")
        if med > args.max_rtt:
            print(f"FAIL: median RTT above {args.max_rtt} ms")
            ok = False
    else:
        print("RTT: no TIMESYNC replies (FC may not answer TIMESYNC)")

    print("PASS" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
