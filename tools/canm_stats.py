#!/usr/bin/env python3
"""canm_stats.py - query the CanMaster's local counters over its PC serial link.

Sends the master-local command [0x02][0x5A] (LEN-framed, never used by
fota_host.py) and prints the ASCII stats line. Use before/after each C2
acceptance run to capture tx/rx/error-frame evidence.

Usage: python tools/canm_stats.py --port COM3
"""
import argparse
import serial

STATS_CMD = bytes([0x02, 0x5A])


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", required=True, help="serial port of the CH340 wired to CanMaster")
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=1.0)
    ser.reset_input_buffer()
    ser.write(STATS_CMD)
    line = ser.read(256)
    ser.close()
    if not line:
        print("no reply - check wiring/port (master only answers when idle)")
        return 1
    print(line.decode("ascii", errors="replace").strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
