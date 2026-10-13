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
    python fota_host.py --port COM5 --slot B upload FOTA_App_B_signed.bin
    python fota_host.py --port COM5 --slot B activate FOTA_App_B_signed.bin
    python fota_host.py --port COM5 monitor --seconds 10   # watch the app heartbeat

    # one-shot demo: reboot into bootloader, erase B, upload, activate
    python fota_host.py --port COM5 --slot B update FOTA_App_B_signed.bin

    # sign a build first (the bootloader rejects unsigned packages):
    python tools/keygen.py                 # once: keypair + Bootloader key header
    python tools/sign.py <image>.bin --sv 1 -o <image>_signed.bin

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
import hashlib
import io
import sys
import struct
import time
from pathlib import Path

# ---------------------------------------------------------------------------
# Memory map & metadata
# ---------------------------------------------------------------------------
META_BASE = 0x08008000
META_SIZE = 24                  # FOTA_Meta_t on flash (v2: + min_sv)

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

# ACTIVATE reply status codes (see Bootloader/Core/Inc/bootloader.h)
ACT_STATUS = {
    0x00: "validation failed (slot/image bounds/vector/CRC)",
    0x01: "success",
    0x10: "no or malformed TLV signature trailer",
    0x11: "digest mismatch (image or security_version altered)",
    0x12: "ECDSA-P256 signature invalid",
    0x13: "security version below minimum (anti-rollback)",
}

# ---------------------------------------------------------------------------
# Signed packages (image + TLV trailer). The trailer format lives in
# tools/fota_tlv.py so the signer, this tool and the MCU bootloader can never
# drift apart (the MCU side is Bootloader/Core/Inc/image_tlv.h).
# ---------------------------------------------------------------------------
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import fota_tlv  # noqa: E402  (tools/fota_tlv.py)

PUBLIC_KEY_PATH = Path(__file__).resolve().parent.parent / "tools" / "fota_public.pem"


class FirmwareFile:
    """A firmware file: bare image or signed package (image + TLV trailer).

    blob   - whole file content, trailer included (what gets uploaded)
    image  - the bare image (what ACTIVATE's image_len/CRC refer to)
    sv     - security version from the trailer (None when unsigned)
    signed - True when a structurally valid trailer terminated the file
    """

    def __init__(self, data: bytes):
        self.blob = data
        parsed = fota_tlv.parse_package(data)
        self.signed = parsed is not None
        if parsed:
            self.image, self.sv, _, _ = parsed
        else:
            self.image, self.sv = data, None
        self.note = ""


def preverify(fw: FirmwareFile) -> None:
    """Host-side package check before touching the MCU (fail fast)."""
    if not fw.signed:
        fw.note = "unsigned image (no TLV trailer) - the bootloader will reject it"
        return
    if not PUBLIC_KEY_PATH.exists():
        fw.note = f"signed package (sv={fw.sv}) - {PUBLIC_KEY_PATH.name} missing, pre-check skipped"
        return
    ok, reason, _, _ = fota_tlv.verify_package(fw.blob, PUBLIC_KEY_PATH.read_bytes())
    fw.note = f"signed package (sv={fw.sv}) - host pre-check: {reason}"
    if not ok:
        raise RuntimeError(f"host-side package verification failed: {reason}")


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
def meta_pack(active, state, image_len=0, image_crc=0, min_sv=0) -> bytes:
    """Serialize FOTA_Meta_t exactly like the C struct (24 bytes, v2)."""
    head = struct.pack("<IBBBB", META_MAGIC, META_VERSION, active, state, 0)
    tail = struct.pack("<III", image_len, image_crc, min_sv)
    return head + tail + struct.pack("<I", crc32_fota(head + tail))


def meta_unpack(raw: bytes):
    if len(raw) != META_SIZE or raw[0] == 0xFF:
        return None
    magic, version, active, state, res, image_len, image_crc, min_sv, meta_crc = struct.unpack("<IBBBBIIII", raw)
    if magic != META_MAGIC or version != META_VERSION:
        return None
    if crc32_fota(raw[:20]) != meta_crc:
        return None
    return dict(active=active, state=state, image_len=image_len, image_crc=image_crc, min_sv=min_sv)


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

    def __init__(self, public_pem: bytes = None):
        self.flash = bytearray(0x100000)          # 1 MB model
        self.flash[:] = b"\xFF" * len(self.flash)
        self.jumped = False
        self.rx = bytearray()
        self.tx = bytearray()
        # Public key used to mirror the MCU's signature check; None disables
        # the ECDSA step (structure + digest are still enforced).
        if public_pem is None and PUBLIC_KEY_PATH.exists():
            public_pem = PUBLIC_KEY_PATH.read_bytes()
        self.public_pem = public_pem
        # Anti-rollback floor: mirrors FOTA_Meta_t.min_sv on the MCU. Raised
        # on every successful activate, never lowered (rollbacks keep it).
        self.min_sv = 0

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

    def _meta_write(self, active, state, image_len=0, image_crc=0, min_sv=None):
        """min_sv=None carries the current floor over (rollback / confirm
        paths); only a successful activate passes an explicit (higher) value."""
        off = META_BASE - 0x08000000
        if min_sv is None:
            min_sv = self.min_sv
        else:
            self.min_sv = min_sv
        self.flash[off : off + 16 * 1024] = b"\xFF" * (16 * 1024)  # sector 2 erase
        self.flash[off : off + META_SIZE] = meta_pack(active, state, image_len, image_crc, min_sv)

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
                        status, sv = self._verify_signed(idx, image_len)
                        if status == 0x01 and sv < self.min_sv:
                            # validly signed but older than the floor
                            status = 0x13
                        if status == 0x01:
                            self._meta_write(slot, META_STATE_TRIAL, image_len, image_crc, min_sv=sv)
                            self.jumped = True
            self.tx.append(status)
        else:
            self.tx.append(NACK)

    def _verify_signed(self, idx: int, image_len: int):
        """Mirror Bootloader_Verify_Signed_Image on the MCU: parse the TLV
        trailer stored right after the image, recompute the digest over
        image || security_version and check the ECDSA-P256 signature.
        Returns (ACT_STATUS code, security_version); sv is 0 when the TLV
        could not be parsed."""
        if idx + image_len + 6 > len(self.flash):
            return 0x10, 0
        head = bytes(self.flash[idx + image_len : idx + image_len + 6])
        if head[:4] != fota_tlv.TLV_MAGIC:
            return 0x10, 0
        (tlv_len,) = struct.unpack_from("<H", head, 4)
        probe = bytes(self.flash[idx + image_len : idx + image_len + 6 + tlv_len])
        parsed = fota_tlv._parse_at(probe, 0)
        if not parsed:
            return 0x10, 0
        _, sv, digest, sig = parsed
        image = bytes(self.flash[idx : idx + image_len])
        if hashlib.sha256(image + struct.pack("<I", sv)).digest() != digest:
            return 0x11, sv
        if self.public_pem:
            try:
                import ecdsa

                vk = ecdsa.VerifyingKey.from_pem(self.public_pem.decode())
                vk.verify_digest(sig, digest, sigdecode=ecdsa.util.sigdecode_string)
            except Exception:
                return 0x12, sv
        return 0x01, sv


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

    def _send_with_logs(self, packet: bytes, reply_len: int) -> bytes:
        """_send, then drain any log text the MCU emits AFTER the reply byte
        (activate sends its status first, then a human-readable line)."""
        reply = self._send(packet, reply_len)
        ser = getattr(self.t, "ser", None)
        if ser is not None:
            old_timeout = ser.timeout
            ser.timeout = 0.3
            try:
                while True:
                    chunk = ser.read(256)
                    if not chunk:
                        break
                    sys.stdout.write(chunk.decode("ascii", errors="replace"))
                    sys.stdout.flush()
            finally:
                ser.timeout = old_timeout
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

    def activate(self, firmware, slot: int = SLOT_A) -> str:
        fw = firmware if isinstance(firmware, FirmwareFile) else FirmwareFile(firmware)
        ok, why = self._image_ok_for_slot(fw.image, slot)
        if not ok:
            return f"Activate Refused: {why}"
        packet = activate_cmd(slot, len(fw.image), crc32_fota(fw.image))
        reply = self._send_with_logs(packet, 1)
        if reply[0] == NACK:
            return "NACK"
        if reply[0] == 0x01:
            return "Activate Success (CRC + signature verified, jumping...)"
        return f"Activate Failed: {ACT_STATUS.get(reply[0], f'status 0x{reply[0]:02X}')}"

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

    def update(self, firmware, slot: int = SLOT_B) -> str:
        """One-shot demo flow: reboot -> erase slot -> upload -> activate."""
        fw = firmware if isinstance(firmware, FirmwareFile) else FirmwareFile(firmware)
        print(self.reboot())
        time.sleep(0.15)
        print(self.erase_slot(slot))
        print(self.upload(fw.blob, slot))
        return self.activate(fw, slot)


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

    # 2) metadata pack/unpack round trip (v2: 24 bytes with min_sv)
    raw = meta_pack(SLOT_A, META_STATE_TRIAL, 700, 0x12345678)
    assert len(raw) == META_SIZE
    meta = meta_unpack(raw)
    assert meta == dict(active=SLOT_A, state=META_STATE_TRIAL, image_len=700, image_crc=0x12345678, min_sv=0)
    print("[PASS] metadata pack/unpack round trip (24 bytes, v2)")

    # The simulator mirrors the production bootloader (BL_REQUIRE_SIGNED_IMAGES):
    # unsigned images and bad signatures are rejected, so every success path
    # signs its package with an ephemeral key pinned into the simulator.
    try:
        import ecdsa
    except ImportError:
        print("selftest requires the 'ecdsa' package: pip install ecdsa")
        return 1
    sk = ecdsa.SigningKey.generate(curve=ecdsa.NIST256p)
    sk_wrong = ecdsa.SigningKey.generate(curve=ecdsa.NIST256p)

    def signed(image: bytes, sv: int = 1, key=None) -> FirmwareFile:
        return FirmwareFile(fota_tlv.build_package(image, sv, key or sk))

    sim = SimulatedSTM32(public_pem=sk.get_verifying_key().to_pem())
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

    # 4) first-time setup on slot A: upload a SIGNED package
    fw_a = fake_firmware(700, 7, 0x20010000, SLOT_A_BASE + 0x100)
    pkg_a = signed(fw_a)
    assert client.erase_slot(SLOT_A) == "Erase Success"
    print("[PASS] Erase slot A (empty metadata) -> Success")
    assert client.upload(pkg_a.blob, SLOT_A) == "Upload Success (818 bytes in 13 chunks)"
    print("[PASS] Upload 700-byte signed package to slot A (image + 118-byte trailer)")
    off_a = SLOT_A_BASE - 0x08000000
    assert bytes(sim.flash[off_a : off_a + 700]) == fw_a
    print("[PASS] Flash content matches the uploaded image part")

    # 5) activate with an image differing from flash -> CRC rejects first
    assert client.activate(FirmwareFile(fw_a[:-1] + b"\x00"), SLOT_A) == (
        "Activate Failed: validation failed (slot/image bounds/vector/CRC)"
    )
    assert sim.jumped is False
    assert sim._meta_read() is None
    print("[PASS] Activate with altered image -> CRC rejected, metadata untouched")

    # 5b) signature enforcement matrix (mirrors the MCU Bootloader_Verify chain)
    # 5b.1 unsigned legacy image: no trailer in flash -> 0x10
    assert client.erase_slot(SLOT_A) == "Erase Success"
    assert client.upload(fw_a, SLOT_A) == "Upload Success (700 bytes in 11 chunks)"
    r = client.activate(FirmwareFile(fw_a), SLOT_A)
    assert r == "Activate Failed: no or malformed TLV signature trailer", r
    print("[PASS] Unsigned legacy image -> rejected 0x10 (no TLV trailer)")

    # 5b.2 tampered image with a consistent CRC: the attacker can always fix
    # the CRC (it is linear) - the digest is what stops them -> 0x11
    _, sv_a, digest_a, sig_a = fota_tlv.parse_package(pkg_a.blob)
    flipped = bytearray(fw_a)
    flipped[600] ^= 0xFF
    tampered = FirmwareFile(bytes(flipped) + fota_tlv.build_trailer(sv_a, digest_a, sig_a))
    assert client.erase_slot(SLOT_A) == "Erase Success"
    assert client.upload(tampered.blob, SLOT_A) == "Upload Success (818 bytes in 13 chunks)"
    r = client.activate(tampered, SLOT_A)
    assert r == "Activate Failed: digest mismatch (image or security_version altered)", r
    print("[PASS] Tampered image with recomputed CRC -> rejected 0x11 (digest mismatch)")

    # 5b.3 attacker re-signs with their own key -> 0x12
    pkg_wrongkey = signed(fw_a, key=sk_wrong)
    assert client.erase_slot(SLOT_A) == "Erase Success"
    assert client.upload(pkg_wrongkey.blob, SLOT_A) == "Upload Success (818 bytes in 13 chunks)"
    r = client.activate(pkg_wrongkey, SLOT_A)
    assert r == "Activate Failed: ECDSA-P256 signature invalid", r
    print("[PASS] Wrong-key signature -> rejected 0x12")

    # 5b.4 one flipped signature byte -> 0x12
    pkg_badsig = FirmwareFile(pkg_a.blob[:-1] + bytes([pkg_a.blob[-1] ^ 0x01]))
    assert client.erase_slot(SLOT_A) == "Erase Success"
    assert client.upload(pkg_badsig.blob, SLOT_A) == "Upload Success (818 bytes in 13 chunks)"
    r = client.activate(pkg_badsig, SLOT_A)
    assert r == "Activate Failed: ECDSA-P256 signature invalid", r
    print("[PASS] Tampered signature byte -> rejected 0x12")

    # 6) activate the genuine package ok -> TRIAL, jumped
    assert client.erase_slot(SLOT_A) == "Erase Success"
    assert client.upload(pkg_a.blob, SLOT_A) == "Upload Success (818 bytes in 13 chunks)"
    assert client.activate(pkg_a, SLOT_A) == "Activate Success (CRC + signature verified, jumping...)"
    assert sim.jumped is True
    assert client.get_slot_status() == (SLOT_A, META_STATE_TRIAL, 700)
    print("[PASS] Activate slot A -> Success, state TRIAL")

    # 7) unconfirmed update + power cycle -> rollback; the old image (A) boots
    assert sim.power_cycle() is True
    assert client.get_slot_status() == (SLOT_A, META_STATE_IDLE, 0)
    print("[PASS] Unconfirmed TRIAL rolled back on power cycle (old slot boots)")

    # 8) activate A again, confirm, power cycle -> stays on A
    assert client.activate(pkg_a, SLOT_A) == "Activate Success (CRC + signature verified, jumping...)"
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
    assert client.upload(pkg_a.blob, SLOT_A) == "Upload Success (818 bytes in 13 chunks)"
    assert client.activate(pkg_a, SLOT_A) == "Activate Success (CRC + signature verified, jumping...)"
    sim.confirm_app()
    print("[PASS] Corrupt active slot can be erased for recovery")

    # 10) update to B without confirmation -> power cycle rolls back to A
    fw_b = fake_firmware(500, 13, 0x20020000 - 0x1000, SLOT_B_BASE + 0x100)
    pkg_b = signed(fw_b)
    assert client.erase_slot(SLOT_B) == "Erase Success"
    assert client.upload(pkg_b.blob, SLOT_B) == "Upload Success (618 bytes in 10 chunks)"
    off_b = SLOT_B_BASE - 0x08000000
    assert bytes(sim.flash[off_b : off_b + 500]) == fw_b
    assert client.activate(pkg_b, SLOT_B) == "Activate Success (CRC + signature verified, jumping...)"
    assert sim.power_cycle() is True            # A boots again
    assert client.get_slot_status() == (SLOT_A, META_STATE_IDLE, 0)
    print("[PASS] Bad update to B rolls back to A on power cycle")

    # 11) update to B + confirm -> power cycle stays on B
    assert client.erase_slot(SLOT_B) == "Erase Success"
    assert client.upload(pkg_b.blob, SLOT_B) == "Upload Success (618 bytes in 10 chunks)"
    assert client.activate(pkg_b, SLOT_B) == "Activate Success (CRC + signature verified, jumping...)"
    sim.confirm_app()
    assert client.get_slot_status() == (SLOT_B, META_STATE_IDLE, 500)
    assert sim.power_cycle() is True
    assert client.get_slot_status() == (SLOT_B, META_STATE_IDLE, 500)
    print("[PASS] Confirmed update to B survives power cycle")

    # 12) optional: upload the real compiled demo application (signed)
    app_bin = args_real_app()
    if app_bin:
        pkg_real = signed(app_bin)
        assert client.erase_slot(SLOT_A) == "Erase Success"
        assert "Upload Success" in client.upload(pkg_real.blob, SLOT_A)
        stored = bytes(sim.flash[off_a : off_a + len(app_bin)])
        assert stored == app_bin
        # wrong slot gate: the slot-A build must not activate into slot B
        assert "not inside slot B" in client.activate(pkg_real, SLOT_B)
        assert client.activate(pkg_real, SLOT_A) == "Activate Success (CRC + signature verified, jumping...)"
        sim.confirm_app()
        assert sim.power_cycle() is True
        assert client.get_slot_status() == (SLOT_A, META_STATE_IDLE, len(app_bin))
        print(f"[PASS] Real FOTA_App.bin ({len(app_bin)} bytes) signed upload + activate + confirm")

        # 13) boot-time whole-image CRC: corrupting a confirmed slot makes it
        # untrusted at power-up, so the other slot boots instead
        sim.flash[off_a + 500] ^= 0xFF
        assert sim.power_cycle() is True
        assert client.get_slot_status() == (SLOT_B, META_STATE_IDLE, 0)
        print("[PASS] Boot-time whole-image CRC catches a corrupted confirmed slot")

        # 14) one-shot update() flow: reboot magic + erase + upload + activate
        # (A is inactive here, so the erase is allowed)
        assert "Activate Success" in client.update(pkg_real, SLOT_A)
        assert sim.jumped is True
        sim.confirm_app()
        assert client.get_slot_status() == (SLOT_A, META_STATE_IDLE, len(app_bin))
        print("[PASS] One-shot update() flow (reboot -> erase -> upload -> activate)")

    # 15) anti-rollback floor (feature B): min_sv rises with each activation,
    # an older-but-validly-signed image is rejected, and rollbacks never
    # lower the floor (rollback != downgrade attack)
    pkg_b2 = signed(fw_b, sv=2)
    assert client.erase_slot(SLOT_B) == "Erase Success"
    assert client.upload(pkg_b2.blob, SLOT_B) == "Upload Success (618 bytes in 10 chunks)"
    assert client.activate(pkg_b2, SLOT_B) == "Activate Success (CRC + signature verified, jumping...)"
    sim.confirm_app()
    assert sim.min_sv == 2
    print("[PASS] Activating sv=2 raises the anti-rollback floor to 2")

    pkg_a1 = signed(fw_a, sv=1)   # attacker replays an older, validly-signed image
    assert client.erase_slot(SLOT_A) == "Erase Success"
    assert client.upload(pkg_a1.blob, SLOT_A) == "Upload Success (818 bytes in 13 chunks)"
    r = client.activate(pkg_a1, SLOT_A)
    assert r == "Activate Failed: security version below minimum (anti-rollback)", r
    print("[PASS] Replay of older signed image (sv=1 < min_sv=2) -> rejected 0x13")

    # rollback != downgrade: an unconfirmed TRIAL rolls the slot back, the
    # floor survives, and the old image is still blocked afterwards
    pkg_a2 = signed(fw_a, sv=2)
    assert client.erase_slot(SLOT_A) == "Erase Success"
    assert client.upload(pkg_a2.blob, SLOT_A) == "Upload Success (818 bytes in 13 chunks)"
    assert client.activate(pkg_a2, SLOT_A) == "Activate Success (CRC + signature verified, jumping...)"
    assert sim.power_cycle() is True                               # unconfirmed -> rolls back to B
    assert client.get_slot_status()[0] == SLOT_B
    assert sim.min_sv == 2
    assert client.erase_slot(SLOT_A) == "Erase Success"
    assert client.upload(pkg_a1.blob, SLOT_A) == "Upload Success (818 bytes in 13 chunks)"
    r = client.activate(pkg_a1, SLOT_A)
    assert r == "Activate Failed: security version below minimum (anti-rollback)", r
    print("[PASS] Rollback does not lower the floor (min_sv stays 2 after TRIAL rollback)")

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
        "--force",
        action="store_true",
        help="skip the host-side package pre-check and let the MCU decide "
        "(for negative tests: tampered packages are rejected by the bootloader)",
    )
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
            fw = FirmwareFile(open(args.file, "rb").read())
            print(client.upload(fw.blob, slot))
        elif args.command == "activate":
            if not args.file:
                print("usage: fota_host.py activate <file.bin> [--slot A|B]")
                return 2
            fw = FirmwareFile(open(args.file, "rb").read())
            if args.force:
                fw.note = f"host pre-check SKIPPED (--force), sv={fw.sv}"
            else:
                preverify(fw)
            print(fw.note)
            print(client.activate(fw, slot))
        elif args.command == "update":
            if not args.file:
                print("usage: fota_host.py update <file.bin> [--slot A|B]")
                return 2
            fw = FirmwareFile(open(args.file, "rb").read())
            if args.force:
                fw.note = f"host pre-check SKIPPED (--force), sv={fw.sv}"
            else:
                preverify(fw)
            print(fw.note)
            print(client.update(fw, slot))
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
