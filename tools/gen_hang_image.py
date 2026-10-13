#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Generate a deliberately hanging firmware image for the IWDG rollback demo.

The image has a valid vector table (so the bootloader accepts and activates
it) but its reset handler is an infinite Thumb loop (`b .` = 0xE7FE): the app
never runs, never feeds the watchdog, so the IWDG resets the MCU and the
bootloader automatically rolls back to the other slot.

Usage: python tools/gen_hang_image.py
Outputs: Host/demo_firmware/hang_a.bin (slot A) and hang_b.bin (slot B)
"""

import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "Host" / "demo_firmware"

SLOT_A_BASE = 0x0800C000
SLOT_B_BASE = 0x08040000
SP = 0x20010000        # inside SRAM -> passes the bootloader's vector check
LOOP_OFFSET = 0x100    # reset handler location inside the image


def hang_image(slot_base: int) -> bytes:
    reset_handler = slot_base + LOOP_OFFSET + 1  # +1: Thumb bit
    image = bytearray(LOOP_OFFSET + 2)
    struct.pack_into("<I", image, 0, SP)
    struct.pack_into("<I", image, 4, reset_handler)
    image[LOOP_OFFSET : LOOP_OFFSET + 2] = b"\xFE\xE7"  # b .  (infinite loop)
    return bytes(image)


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    for name, base in (("hang_a.bin", SLOT_A_BASE), ("hang_b.bin", SLOT_B_BASE)):
        data = hang_image(base)
        (OUT / name).write_bytes(data)
        reset = struct.unpack_from("<I", data, 4)[0]
        print(f"{OUT / name}: {len(data)} bytes, reset handler at 0x{reset:08X}")


if __name__ == "__main__":
    main()
