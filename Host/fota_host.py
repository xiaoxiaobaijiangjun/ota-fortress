#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
FOTA-STM32 Host Tool (dual-slot A/B)
====================================

Talks the FOTA bootloader protocol over a serial port (or an in-Python
simulated STM32) so the whole update chain can be demonstrated without
an ESP32, Firebase or Node-RED:

    python fota_host.py --port COM5 status
    python fota_host.py --port COM5 --slot B erase
    python fota_host.py --port COM5 --slot B upload FOTA_App_B.bin
    python fota_host.py --port COM5 --slot B activate FOTA_App_B.bin
    python fota_host.py --port COM5 monitor --seconds 10   # watch the app heartbeat

    # one-shot demo: reboot into bootloader, erase B, upload, activate
    python fota_host.py --port COM5 --slot B update FOTA_App_B.bin

    # protocol self-test against a simulated bootloader (no hardware needed)
    python fota_host.py selftest

Commands: id, rdp, status, erase, upload <file.bin>, activate <file.bin>,
          jump, update <file.bin>, reboot, monitor, crc <file.bin>, list

Memory map (STM32F407VE, 512 KB flash):
    0x08000000  Bootloader (32 KB, sectors 0-1)
    0x08008000  Metadata    (16 KB, sector 2)
    0x0800C000  Slot A      (208 KB, sectors 3-5)
    0x08040000  Slot B      (256 KB, sectors 6-7)

The packet format and CRC-32/MPEG-2 match the STM32 bootloader and the
original ESP32 sketch (EhabMagdyy/FOTA-STM32).
"""

import argparse
import io
import sys
import struct
import time

# ---------------------------------------------------------------------------
# Memory map & metadata
# ---------------------------------------------------------------------------
META_BASE = 0x08008000
META_SIZE = 20                  # FOTA_Meta_t on flash

SLOT_A_BASE = 0x0800C000
SLOT_A_SIZE = 0x34000           # sectors 3-5 (208 KB)
SLOT_B_BASE = 0x08040000
SLOT_B_SIZE = 0x40000           # sectors 6-7 (256 KB)

SLOT_A = 0
SLOT_B = 1

META_MAGIC = 0x464F5441         # "FOTA"
META_VERSION = 1
META_STATE_EMPTY = 0xFF
META_STATE_IDLE = 0x01
META_STATE_TRIAL = 0x02

CHUNK_SIZE = 64
NACK = 0xAB
APP_REBOOT_MAGIC = 0xAA

CMD_GET_CID = 0x10
CMD_GET_RDP = 0x11
CMD_JUMP = 0x12
CMD_ERASE = 0x13
CMD_WRITE = 0x14
CMD_GET_STATUS = 0x15
CMD_ERASE_SLOT = 0x16
CMD_ACTIVATE = 0x17


def slot_range(slot):
    """Return (base, size) for a slot, raising on invalid values."""
    if slot == SLOT_A:
        return SLOT_A_BASE, SLOT_A_SIZE
    if slot == SLOT_B:
        return SLOT_B_BASE, SLOT_B_SIZE
    raise ValueError(f"invalid slot: {slot}")


# ---------------------------------------------------------------------------
# FOTA CRC32 - bitwise implementation identical to the original ESP32 sketch
# (init 0xFFFFFFFF, poly 0x04C11DB7, byte XORed into the low byte, 32 shifts,
# no reflection, no final XOR).
#
# This is NOT the standard CRC-32/MPEG-2; it is the algorithm produced by the
# STM32 hardware CRC peripheral when HAL_CRC_Accumulate() is fed one byte per
# 32-bit word with default settings. Both sides were verified numerically:
#   esp32_sketch_crc("123456789") == stm32_hw_model("123456789")
#                                 == 0x1556F485
# ---------------------------------------------------------------------------
def crc32_fota(data: bytes) -> int:
    crc = 0xFFFFFFFF
    poly = 0x04C11DB7
    for byte in data:
        crc ^= byte
        for _ in range(32):
            if crc & 0x80000000:
                crc = ((crc << 1) ^ poly) & 0xFFFFFFFF
            else:
                crc = (crc << 1) & 0xFFFFFFFF
    return crc


def _stm32_hw_crc_model(data: bytes) -> int:
    """Model of the STM32 CRC peripheral (RM0090): one 32-bit word per byte,
    MSB-first bit processing, no reversal, init 0xFFFFFFFF, poly 0x04C11DB7."""
    poly = 0x04C11DB7
    crc = 0xFFFFFFFF
    for byte in data:
        word = byte
        for i in range(31, -1, -1):
            feedback = ((word >> i) & 1) ^ ((crc >> 31) & 1)
            crc = ((crc << 1) ^ (poly if feedback else 0)) & 0xFFFFFFFF
    return crc


# ---------------------------------------------------------------------------
# Metadata helpers
# ---------------------------------------------------------------------------
def meta_pack(active, state, image_len=0, image_crc=0) -> bytes:
    """Serialize FOTA_Meta_t exactly like the C struct (20 bytes)."""
    head = struct.pack("<IBBBB", META_MAGIC, META_VERSION, active, state, 0)
    tail = struct.pack("<II", image_len, image_crc)
    return head + tail + struct.pack("<I", crc32_fota(head + tail))


def meta_unpack(raw: bytes):
    if len(raw) != META_SIZE or raw[0] == 0xFF:
        return None
    magic, version, active, state, res, image_len, image_crc, meta_crc = struct.unpack("<IBBBBIII", raw)
    if magic != META_MAGIC or version != META_VERSION:
        return None
    if crc32_fota(raw[:16]) != meta_crc:
        return None
    return dict(active=active, state=state, image_len=image_len, image_crc=image_crc)


# ---------------------------------------------------------------------------
# Packet builders
# ---------------------------------------------------------------------------
def simple_cmd(command: int) -> bytes:
    """[5][cmd][crc32 over first 2 bytes, little-endian]"""
    head = bytes([5, command])
    return head + struct.pack("<I", crc32_fota(head))


def write_cmd(address: int, payload: bytes) -> bytes:
    """[74][0x14][addr LE][64][64 data bytes][crc32 over first 71 bytes]"""
    assert len(payload) == CHUNK_SIZE
    head = bytes([74, CMD_WRITE]) + struct.pack("<I", address) + bytes([CHUNK_SIZE])
    body = head + payload
    return body + struct.pack("<I", crc32_fota(body))


def erase_slot_cmd(slot: int) -> bytes:
    """[6][0x16][slot][crc32 over first 3 bytes]"""
    head = bytes([6, CMD_ERASE_SLOT, slot])
    return head + struct.pack("<I", crc32_fota(head))


def activate_cmd(slot: int, image_len: int, image_crc: int) -> bytes:
    """[14][0x17][slot][image_len LE32][image_crc LE32][crc32 over first 11 bytes]"""
    head = bytes([14, CMD_ACTIVATE, slot]) + struct.pack("<II", image_len, image_crc)
    return head + struct.pack("<I", crc32_fota(head))


# ---------------------------------------------------------------------------
# Transport: real serial port or in-memory simulated link
# ---------------------------------------------------------------------------
class SerialTransport:
    def __init__(self, port: str, baud: int):
        import serial

        # 5 s: the activate command verifies the whole image CRC on the MCU
        self.ser = serial.Serial(port, baud, timeout=5.0)
        self.ser.reset_input_buffer()
        self.ser.reset_output_buffer()
        time.sleep(0.1)

    def write(self, data: bytes) -> None:
        self.ser.write(data)
        self.ser.flush()

    def read(self, n: int) -> bytes:
        return self.ser.read(n)

    def close(self) -> None:
        self.ser.close()


class SimulatedSTM32:
    """Software model of the dual-slot FOTA bootloader (protocol only).
    Used by --sim / selftest to validate the host tool without hardware."""

    CHIP_ID = 0x413  # STM32F407

    def __init__(self):
        self.flash = bytearray(0x100000)          # 1 MB model
        self.flash[:] = b"\xFF" * len(self.flash)
        self.jumped = False
        self.rx = bytearray()
        self.tx = bytearray()

    # -- wire interface -----------------------------------------------------
    def write(self, data: bytes) -> None:
        self.rx.extend(data)
        while len(self.rx) > 0:
            length = self.rx[0]
            # Mirror the MCU: an out-of-range length byte is NACKed and
            # consumed immediately (e.g. the 0xAA reboot magic).
            if length == 0 or length > 99:
                del self.rx[0]
                self.tx.append(NACK)
                continue
            if len(self.rx) < length + 1:
                break
            packet = bytes(self.rx[: length + 1])
            del self.rx[: length + 1]
            self._handle(packet)

    def read(self, n: int) -> bytes:
        out = bytes(self.tx[:n])
        del self.tx[:n]
        return out

    # -- metadata -----------------------------------------------------------
    def _meta_read(self):
        off = META_BASE - 0x08000000
        return meta_unpack(bytes(self.flash[off : off + META_SIZE]))

    def _meta_write(self, active, state, image_len=0, image_crc=0):
        off = META_BASE - 0x08000000
        self.flash[off : off + 16 * 1024] = b"\xFF" * (16 * 1024)  # sector 2 erase
        self.flash[off : off + META_SIZE] = meta_pack(active, state, image_len, image_crc)

    def confirm_app(self):
        """Simulate the app writing TRIAL -> IDLE after running healthy."""
        meta = self._meta_read()
        if meta and meta["state"] == META_STATE_TRIAL:
            self._meta_write(meta["active"], META_STATE_IDLE, meta["image_len"], meta["image_crc"])

    def power_cycle(self) -> bool:
        """Simulate reset + auto-boot with no HOST activity."""
        self.jumped = False
        slot = self._resolve_boot_slot()
        base, size = slot_range(slot)
        ok = self._app_trusted(slot, base, base + size)
        if not ok:
            slot = SLOT_B if slot == SLOT_A else SLOT_A
            base, size = slot_range(slot)
            ok = self._app_trusted(slot, base, base + size)
            if ok:
                self._meta_write(slot, META_STATE_IDLE)
        self.jumped = ok
        return ok

    # -- internal helpers ---------------------------------------------------
    def _resolve_boot_slot(self) -> int:
        meta = self._meta_read()
        if meta:
            if meta["state"] == META_STATE_TRIAL:
                # unconfirmed update -> roll back to the other slot
                other = SLOT_B if meta["active"] == SLOT_A else SLOT_A
                self._meta_write(other, META_STATE_IDLE)
                return other
            return SLOT_A if meta["active"] != SLOT_B else SLOT_B
        return SLOT_A

    def _app_valid(self, base: int, end: int) -> bool:
        idx = base - 0x08000000
        msp = struct.unpack("<I", bytes(self.flash[idx : idx + 4]))[0]
        reset = struct.unpack("<I", bytes(self.flash[idx + 4 : idx + 8]))[0]
        return 0x20000000 <= msp < 0x20020000 and base <= reset < end

    def _app_trusted(self, slot: int, base: int, end: int) -> bool:
        """Vector sanity plus whole-image CRC when the metadata recorded one
        (mirrors Bootloader_App_Trusted on the MCU)."""
        if not self._app_valid(base, end):
            return False
        meta = self._meta_read()
        if meta and meta["active"] == slot and 8 <= meta["image_len"] <= (end - base):
            idx = base - 0x08000000
            calc = crc32_fota(bytes(self.flash[idx : idx + meta["image_len"]]))
            return calc == meta["image_crc"]
        return True

    def _write_allowed(self, address: int, length: int) -> bool:
        if 0x20000000 <= address <= 0x20020000:
            return True
        for base, size in ((SLOT_A_BASE, SLOT_A_SIZE), (SLOT_B_BASE, SLOT_B_SIZE)):
            if base <= address and address + length <= base + size:
                return True
        return False

    # -- protocol handlers --------------------------------------------------
    def _check_crc(self, packet: bytes) -> bool:
        host_crc = struct.unpack("<I", packet[-4:])[0]
        return crc32_fota(packet[:-4]) == host_crc

    def _handle(self, packet: bytes) -> None:
        if len(packet) < 2 or packet[0] != len(packet) - 1:
            self.tx.append(NACK)
            return
        if not self._check_crc(packet):
            self.tx.append(NACK)
            return

        cmd = packet[1]
        if cmd == CMD_GET_CID:
            self.tx += struct.pack("<H", self.CHIP_ID)
        elif cmd == CMD_GET_RDP:
            self.tx.append(0x00)  # protection level 0
        elif cmd == CMD_GET_STATUS:
            meta = self._meta_read()
            if meta:
                reply = bytes([meta["active"], meta["state"]]) + struct.pack("<I", meta["image_len"])
            else:
                reply = bytes([SLOT_A, META_STATE_EMPTY]) + struct.pack("<I", 0)
            self.tx += reply
        elif cmd == CMD_JUMP:
            slot = self._resolve_boot_slot()
            base, size = slot_range(slot)
            ok = self._app_trusted(slot, base, base + size)
            if not ok:
                slot = SLOT_B if slot == SLOT_A else SLOT_A
                base, size = slot_range(slot)
                ok = self._app_trusted(slot, base, base + size)
                if ok:
                    self._meta_write(slot, META_STATE_IDLE)
            self.tx.append(0x01 if ok else 0x00)
            self.jumped = ok
        elif cmd == CMD_ERASE:  # legacy: erase slot A
            meta = self._meta_read()
            refuse = False
            if meta and meta["active"] == SLOT_A:
                if self._app_trusted(SLOT_A, SLOT_A_BASE, SLOT_A_BASE + SLOT_A_SIZE):
                    refuse = True
            if refuse:
                self.tx.append(0x00)  # refuse: active slot
            else:
                base, size = slot_range(SLOT_A)
                off = base - 0x08000000
                self.flash[off : off + size] = b"\xFF" * size
                self.tx.append(0x03)
        elif cmd == CMD_ERASE_SLOT:
            slot = packet[2] if len(packet) > 2 else 0xFF
            meta = self._meta_read()
            refuse = False
            if meta and slot == meta["active"] and slot in (SLOT_A, SLOT_B):
                base, size = slot_range(slot)
                if self._app_trusted(slot, base, base + size):
                    refuse = True
            if refuse:
                self.tx.append(0x00)  # refuse: active slot
            elif slot in (SLOT_A, SLOT_B):
                base, size = slot_range(slot)
                off = base - 0x08000000
                self.flash[off : off + size] = b"\xFF" * size
                self.tx.append(0x03)
            else:
                self.tx.append(0x00)
        elif cmd == CMD_WRITE:
            address = struct.unpack("<I", packet[2:6])[0]
            length = packet[6]
            payload = packet[7 : 7 + length]
            if self._write_allowed(address, length):
                idx = address - 0x08000000
                self.flash[idx : idx + length] = payload
                self.tx.append(0x01)
            else:
                self.tx.append(0x00)
        elif cmd == CMD_ACTIVATE:
            slot = packet[2]
            image_len = struct.unpack("<I", packet[3:7])[0]
            image_crc = struct.unpack("<I", packet[7:11])[0]
            status = 0x00
            if slot in (SLOT_A, SLOT_B):
                base, size = slot_range(slot)
                if 8 <= image_len <= size and self._app_valid(base, base + size):
                    idx = base - 0x08000000
                    calc = crc32_fota(bytes(self.flash[idx : idx + image_len]))
                    if calc == image_crc:
                        self._meta_write(slot, META_STATE_TRIAL, image_len, image_crc)
                        status = 0x01
                        self.jumped = True
            self.tx.append(status)
        else:
            self.tx.append(NACK)


# ---------------------------------------------------------------------------
# Client
# ---------------------------------------------------------------------------
class FOTAClient:
    def __init__(self, transport):
        self.t = transport

    def _send(self, packet: bytes, reply_len: int) -> bytes:
        self.t.write(packet)
        reply = self.t.read(reply_len)
        if len(reply) != reply_len:
            raise RuntimeError(f"timeout waiting for reply (got {len(reply)}/{reply_len} bytes)")
        return reply

    def get_chip_id(self) -> str:
        reply = self._send(simple_cmd(CMD_GET_CID), 2)
        if reply[0] == NACK:
            return "NACK"
        return f"0x{reply[1]:02X}{reply[0]:02X}"

    def read_rdp(self) -> str:
        reply = self._send(simple_cmd(CMD_GET_RDP), 1)
        return "NACK" if reply[0] == NACK else f"Level {reply[0]}"

    def get_slot_status(self):
        """Return (active_slot, state, image_len) or None on NACK."""
        reply = self._send(simple_cmd(CMD_GET_STATUS), 6)
        if reply[0] == NACK:
            return None
        active, state = reply[0], reply[1]
        image_len = struct.unpack("<I", reply[2:6])[0]
        return active, state, image_len

    def status(self) -> str:
        s = self.get_slot_status()
        if s is None:
            return "NACK"
        active, state, image_len = s
        slot_name = "A" if active == SLOT_A else "B"
        state_name = {
            META_STATE_EMPTY: "EMPTY",
            META_STATE_IDLE: "IDLE",
            META_STATE_TRIAL: "TRIAL",
        }.get(state, f"0x{state:02X}")
        return f"Active slot: {slot_name} | State: {state_name} | Last image: {image_len} bytes"

    def erase(self) -> str:
        """Legacy command: erase slot A."""
        return self.erase_slot(SLOT_A)

    def erase_slot(self, slot: int) -> str:
        reply = self._send(erase_slot_cmd(slot), 1)
        if reply[0] == NACK:
            return "NACK"
        if reply[0] == 0x03:
            return "Erase Success"
        if reply[0] == 0x00:
            return "Erase Refused (invalid or active slot)"
        return "Erase Failed"

    def upload(self, firmware: bytes, slot: int = SLOT_A) -> str:
        base, size = slot_range(slot)
        if len(firmware) > size:
            raise RuntimeError(f"firmware too large: {len(firmware)} > {size} bytes")
        # pad last chunk with 0xFF like the ESP32 path does
        padded = firmware + b"\xFF" * ((CHUNK_SIZE - len(firmware) % CHUNK_SIZE) % CHUNK_SIZE)
        chunks = len(padded) // CHUNK_SIZE
        for i in range(chunks):
            address = base + i * CHUNK_SIZE
            reply = self._send(write_cmd(address, padded[i * CHUNK_SIZE : (i + 1) * CHUNK_SIZE]), 1)
            if reply[0] != 0x01:
                return f"Upload Failed at chunk {i + 1}/{chunks} (addr 0x{address:X})"
            if (i + 1) % 32 == 0 or i + 1 == chunks:
                print(f"  chunk {i + 1}/{chunks} @ 0x{address:X} OK")
        return f"Upload Success ({len(firmware)} bytes in {chunks} chunks)"

    @staticmethod
    def _image_ok_for_slot(firmware: bytes, slot: int):
        """Host-side sanity check: the image vector table must point inside
        the target slot (catches uploading the slot-A build into slot B)."""
        base, size = slot_range(slot)
        if len(firmware) < 8:
            return False, "image too small (< 8 bytes)"
        msp = struct.unpack("<I", firmware[:4])[0]
        reset = struct.unpack("<I", firmware[4:8])[0]
        if not (0x20000000 <= msp < 0x20020000):
            return False, f"stack pointer 0x{msp:08X} not in SRAM"
        if not (base <= reset < base + size):
            name = "A" if slot == SLOT_A else "B"
            return False, f"reset vector 0x{reset:08X} not inside slot {name} (0x{base:X}-0x{base + size:X})"
        return True, ""

    def activate(self, firmware: bytes, slot: int = SLOT_A) -> str:
        ok, why = self._image_ok_for_slot(firmware, slot)
        if not ok:
            return f"Activate Refused: {why}"
        packet = activate_cmd(slot, len(firmware), crc32_fota(firmware))
        reply = self._send(packet, 1)
        if reply[0] == NACK:
            return "NACK"
        if reply[0] == 0x01:
            return "Activate Success (image verified, jumping...)"
        return "Activate Failed (image CRC/validation error)"

    def jump(self) -> str:
        reply = self._send(simple_cmd(CMD_JUMP), 1)
        if reply[0] == NACK:
            return "NACK"
        return "Jump Success" if reply[0] == 0x01 else "Jump Failed (no valid app)"

    def reboot(self) -> str:
        """Send the UART magic byte: a running app resets into the bootloader.

        Drain whatever the MCU answers right after the reset (the bootloader's
        NACK if we were already in the bootloader, or its startup banner if a
        running app just reset) so the NEXT command never reads a stale byte
        as its own reply.
        """
        self.t.write(bytes([APP_REBOOT_MAGIC]))
        if isinstance(self.t, SerialTransport):
            old_timeout = self.t.ser.timeout
            # The banner/NACK arrives within a few ms; keep the drain short so
            # the NEXT command still fits inside the bootloader's 1 s window.
            self.t.ser.timeout = 0.1
            try:
                while self.t.read(256):
                    pass
            finally:
                self.t.ser.timeout = old_timeout
        else:
            self.t.read(256)
        return "Reset signal sent (0xAA) - MCU is entering the bootloader window"

    def update(self, firmware: bytes, slot: int = SLOT_B) -> str:
        """One-shot demo flow: reboot -> erase slot -> upload -> activate."""
        print(self.reboot())
        time.sleep(0.15)
        print(self.erase_slot(slot))
        print(self.upload(firmware, slot))
        return self.activate(firmware, slot)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def build_transport(args):
    if args.sim:
        return SimulatedSTM32()
    return SerialTransport(args.port, args.baud)


def cmd_monitor(transport, seconds: float) -> None:
    """Raw serial monitor: print whatever the MCU sends (e.g. the app
    heartbeat after `jump`). Press Ctrl+C to stop early."""
    print(f"Monitoring serial output for {seconds:.0f}s (Ctrl+C to stop)...")
    deadline = time.time() + seconds
    try:
        while time.time() < deadline:
            data = transport.read(256)
            if data:
                text = data.decode("ascii", errors="replace")
                sys.stdout.write(text)
                sys.stdout.flush()
    except KeyboardInterrupt:
        pass
    print()


def cmd_loopback(transport) -> None:
    """Loopback test: send a known pattern and see if it comes back.
    Short the USB-TTL module's TXD and RXD pins first.
    - Got the pattern back  -> module/COM port/driver are fine, problem is on the board side
    - Nothing came back     -> module, cable or COM port problem"""
    packet = bytes([0xAA, 0x55, 0x01, 0x02, 0x03, 0x04])
    transport.write(packet)
    reply = transport.read(6)
    if reply == packet:
        print("Loopback OK: module echoes back exactly (AA 55 01 02 03 04)")
        print("-> module/COM port/driver are fine. Problem is on the board side (wiring or bootloader).")
    elif reply:
        got = " ".join(f"{b:02X}" for b in reply)
        print(f"Got {len(reply)} bytes back (not matching): {got}")
    else:
        print("No bytes received. Check: module plugged in? COM port correct? driver OK?")


def cmd_list_ports():
    try:
        from serial.tools import list_ports

        ports = list_ports.comports()
        if not ports:
            print("No serial ports found.")
            return
        for p in ports:
            print(f"{p.device}\t{p.description}")
    except ImportError:
        print("pyserial not installed: pip install pyserial")


def fake_firmware(length: int, seed: int, sp: int, reset: int) -> bytes:
    """Build a synthetic image with a sane vector table for selftest."""
    assert length >= 8
    hdr = struct.pack("<II", sp, reset)
    body = bytes((i * seed + 3) & 0xFF for i in range(length - 8))
    return hdr + body


def run_selftest() -> int:
    print("== FOTA dual-slot protocol self-test (simulated STM32) ==")

    # 1) CRC known answer + equivalence with the STM32 CRC hardware model
    assert crc32_fota(b"123456789") == 0x1556F485, "CRC vector mismatch"
    assert crc32_fota(b"123456789") == _stm32_hw_crc_model(b"123456789")
    print("[PASS] FOTA CRC32 vector 123456789 -> 0x1556F485")
    print("[PASS] software CRC == STM32 hardware CRC model")

    # 2) metadata pack/unpack round trip
    raw = meta_pack(SLOT_A, META_STATE_TRIAL, 700, 0x12345678)
    assert len(raw) == META_SIZE
    meta = meta_unpack(raw)
    assert meta == dict(active=SLOT_A, state=META_STATE_TRIAL, image_len=700, image_crc=0x12345678)
    print("[PASS] metadata pack/unpack round trip (20 bytes)")

    sim = SimulatedSTM32()
    client = FOTAClient(sim)

    # 3) simple commands
    assert client.get_chip_id() == "0x0413", client.get_chip_id()
    print("[PASS] Get Chip ID -> 0x0413")
    assert client.read_rdp() == "Level 0"
    print("[PASS] Read RDP -> Level 0")
    assert client.get_slot_status() == (SLOT_A, META_STATE_EMPTY, 0)
    assert "EMPTY" in client.status()
    print("[PASS] Slot status on fresh flash -> A / EMPTY")
    assert client.jump() == "Jump Failed (no valid app)"
    print("[PASS] Jump with empty flash -> rejected")

    # 4) first-time setup on slot A
    fw_a = fake_firmware(700, 7, 0x20010000, SLOT_A_BASE + 0x100)
    assert client.erase_slot(SLOT_A) == "Erase Success"
    print("[PASS] Erase slot A (empty metadata) -> Success")
    assert client.upload(fw_a, SLOT_A) == "Upload Success (700 bytes in 11 chunks)"
    print("[PASS] Upload 700-byte synthetic firmware to slot A")
    off_a = SLOT_A_BASE - 0x08000000
    assert bytes(sim.flash[off_a : off_a + 700]) == fw_a
    print("[PASS] Flash content matches uploaded firmware")

    # 5) activate with a corrupt image CRC -> refused, nothing jumped
    assert client.activate(fw_a[:-1] + b"\x00", SLOT_A) == "Activate Failed (image CRC/validation error)"
    assert sim.jumped is False
    assert sim._meta_read() is None
    print("[PASS] Activate with bad image CRC -> rejected, metadata untouched")

    # 6) activate ok -> TRIAL, jumped
    assert client.activate(fw_a, SLOT_A) == "Activate Success (image verified, jumping...)"
    assert sim.jumped is True
    assert client.get_slot_status() == (SLOT_A, META_STATE_TRIAL, 700)
    print("[PASS] Activate slot A -> Success, state TRIAL")

    # 7) unconfirmed update + power cycle -> rollback; the old image (A) boots
    assert sim.power_cycle() is True
    assert client.get_slot_status() == (SLOT_A, META_STATE_IDLE, 0)
    print("[PASS] Unconfirmed TRIAL rolled back on power cycle (old slot boots)")

    # 8) activate A again, confirm, power cycle -> stays on A
    assert client.activate(fw_a, SLOT_A) == "Activate Success (image verified, jumping...)"
    sim.confirm_app()
    assert client.get_slot_status() == (SLOT_A, META_STATE_IDLE, 700)
    assert sim.power_cycle() is True
    assert client.get_slot_status() == (SLOT_A, META_STATE_IDLE, 700)
    print("[PASS] Confirmed update survives power cycle")

    # 9) erasing the active slot is refused
    assert client.erase_slot(SLOT_A) == "Erase Refused (invalid or active slot)"
    assert bytes(sim.flash[off_a : off_a + 700]) == fw_a
    print("[PASS] Erase of the active slot refused, image intact")

    # 9b) a corrupt active slot must remain erasable (recovery path)
    sim.flash[off_a : off_a + 8] = b"\x00" * 8  # destroy the vector table
    assert client.erase_slot(SLOT_A) == "Erase Success"
    assert client.upload(fw_a, SLOT_A) == "Upload Success (700 bytes in 11 chunks)"
    assert client.activate(fw_a, SLOT_A) == "Activate Success (image verified, jumping...)"
    sim.confirm_app()
    print("[PASS] Corrupt active slot can be erased for recovery")

    # 10) update to B without confirmation -> power cycle rolls back to A
    fw_b = fake_firmware(500, 13, 0x20020000 - 0x1000, SLOT_B_BASE + 0x100)
    assert client.erase_slot(SLOT_B) == "Erase Success"
    assert client.upload(fw_b, SLOT_B) == "Upload Success (500 bytes in 8 chunks)"
    off_b = SLOT_B_BASE - 0x08000000
    assert bytes(sim.flash[off_b : off_b + 500]) == fw_b
    assert client.activate(fw_b, SLOT_B) == "Activate Success (image verified, jumping...)"
    assert sim.power_cycle() is True            # A boots again
    assert client.get_slot_status() == (SLOT_A, META_STATE_IDLE, 0)
    print("[PASS] Bad update to B rolls back to A on power cycle")

    # 11) update to B + confirm -> power cycle stays on B
    assert client.erase_slot(SLOT_B) == "Erase Success"
    assert client.upload(fw_b, SLOT_B) == "Upload Success (500 bytes in 8 chunks)"
    assert client.activate(fw_b, SLOT_B) == "Activate Success (image verified, jumping...)"
    sim.confirm_app()
    assert client.get_slot_status() == (SLOT_B, META_STATE_IDLE, 500)
    assert sim.power_cycle() is True
    assert client.get_slot_status() == (SLOT_B, META_STATE_IDLE, 500)
    print("[PASS] Confirmed update to B survives power cycle")

    # 12) optional: upload the real compiled demo application
    app_bin = args_real_app()
    if app_bin:
        assert client.erase_slot(SLOT_A) == "Erase Success"
        assert "Upload Success" in client.upload(app_bin, SLOT_A)
        stored = bytes(sim.flash[off_a : off_a + len(app_bin)])
        assert stored == app_bin
        # wrong slot gate: the slot-A build must not activate into slot B
        assert "not inside slot B" in client.activate(app_bin, SLOT_B)
        assert client.activate(app_bin, SLOT_A) == "Activate Success (image verified, jumping...)"
        sim.confirm_app()
        assert sim.power_cycle() is True
        assert client.get_slot_status() == (SLOT_A, META_STATE_IDLE, len(app_bin))
        print(f"[PASS] Real FOTA_App.bin ({len(app_bin)} bytes) dual-slot upload + activate + confirm")

        # 13) boot-time whole-image CRC: corrupting a confirmed slot makes it
        # untrusted at power-up, so the other slot boots instead
        sim.flash[off_a + 500] ^= 0xFF
        assert sim.power_cycle() is True
        assert client.get_slot_status() == (SLOT_B, META_STATE_IDLE, 0)
        print("[PASS] Boot-time whole-image CRC catches a corrupted confirmed slot")

        # 14) one-shot update() flow: reboot magic + erase + upload + activate
        # (A is inactive here, so the erase is allowed)
        assert "Activate Success" in client.update(app_bin, SLOT_A)
        assert sim.jumped is True
        sim.confirm_app()
        assert client.get_slot_status() == (SLOT_A, META_STATE_IDLE, len(app_bin))
        print("[PASS] One-shot update() flow (reboot -> erase -> upload -> activate)")

    print("\nAll self-tests passed.")
    return 0


def args_real_app():
    from pathlib import Path

    candidates = [
        Path(__file__).resolve().parent.parent / "App" / "MDK-ARM" / "Objects" / "FOTA_App.bin",
        Path(__file__).resolve().parent / "demo_firmware" / "Application.bin",
    ]
    for c in candidates:
        if c.exists():
            return c.read_bytes()
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description="FOTA-STM32 host tool (dual-slot A/B)")
    parser.add_argument("--port", default="COM5", help="serial port (e.g. COM5)")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--sim", action="store_true", help="use simulated bootloader instead of serial port")
    parser.add_argument("--seconds", type=float, default=10.0, help="monitor duration in seconds (default 10)")
    parser.add_argument("--slot", choices=["A", "B"], default="A", help="target slot (default A)")
    parser.add_argument(
        "command",
        nargs="?",
        help="id | rdp | status | erase | upload <bin> | activate <bin> | jump | update <bin> | reboot | crc <bin> | list | selftest",
    )
    parser.add_argument("file", nargs="?", help="firmware .bin file for upload / activate / update / crc")
    args = parser.parse_args()

    slot = SLOT_A if args.slot == "A" else SLOT_B

    if args.command == "list":
        cmd_list_ports()
        return 0
    if args.command == "loopback":
        transport = build_transport(args)
        try:
            cmd_loopback(transport)
        finally:
            transport.close() if hasattr(transport, "close") else None
        return 0
    if args.command == "monitor":
        transport = build_transport(args)
        try:
            cmd_monitor(transport, args.seconds)
        finally:
            transport.close() if hasattr(transport, "close") else None
        return 0
    if args.command == "selftest":
        return run_selftest()
    if args.command == "crc":
        if not args.file:
            print("usage: fota_host.py crc <file.bin>")
            return 2
        data = open(args.file, "rb").read()
        print(f"FOTA CRC32: 0x{crc32_fota(data):08X}")
        return 0
    if args.command is None:
        parser.print_help()
        return 0

    transport = build_transport(args)
    client = FOTAClient(transport)
    try:
        if args.command == "id":
            print("Chip ID:", client.get_chip_id())
        elif args.command == "rdp":
            print("Read Protection:", client.read_rdp())
        elif args.command == "status":
            print(client.status())
        elif args.command == "erase":
            print(client.erase_slot(slot))
        elif args.command == "upload":
            if not args.file:
                print("usage: fota_host.py upload <file.bin> [--slot A|B]")
                return 2
            firmware = open(args.file, "rb").read()
            print(client.upload(firmware, slot))
        elif args.command == "activate":
            if not args.file:
                print("usage: fota_host.py activate <file.bin> [--slot A|B]")
                return 2
            firmware = open(args.file, "rb").read()
            print(client.activate(firmware, slot))
        elif args.command == "update":
            if not args.file:
                print("usage: fota_host.py update <file.bin> [--slot A|B]")
                return 2
            firmware = open(args.file, "rb").read()
            print(client.update(firmware, slot))
        elif args.command == "reboot":
            print(client.reboot())
        elif args.command == "jump":
            print(client.jump())
        else:
            print(f"unknown command: {args.command}")
            return 2
    finally:
        transport.close() if hasattr(transport, "close") else None
    return 0


if __name__ == "__main__":
    sys.exit(main())
