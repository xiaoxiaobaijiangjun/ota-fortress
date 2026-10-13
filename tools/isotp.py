"""Minimal classic ISO-TP segmentation, shared by the CAN bridge transport in
Host/fota_host.py and the protocol selftest. Mirrors the MCU implementation
in Bootloader/Core/Src/can_transport.c:

    SF: [0x0L][<=7 bytes]                  (L = payload length)
    FF: [0x1L][len LO][first 6 bytes]      (12-bit total length)
    CF: [0x2N][7 bytes]                    (N = sequence number, mod 16)
    FC: [0x30][BS][STmin]                  (receiver -> sender; BS=8, STmin=1ms)

CAN ids: request 0x6E0 (host -> device), response 0x6E1 (device -> host).
"""

FC_BS = 8
FC_STMIN = 1


def segment(packet: bytes):
    """Split one packet into 8-byte CAN frames."""
    if len(packet) <= 7:
        f = bytearray(8)
        f[0] = len(packet)
        f[1 : 1 + len(packet)] = packet
        return [bytes(f)]
    frames = []
    ff = bytearray(8)
    ff[0] = 0x10 | (len(packet) >> 8)
    ff[1] = len(packet) & 0xFF
    ff[2:8] = packet[0:6]
    frames.append(bytes(ff))
    sn = 0
    off = 6
    while off < len(packet):
        n = min(7, len(packet) - off)
        sn = (sn + 1) & 0x0F
        cf = bytearray(8)
        cf[0] = 0x20 | sn
        cf[1 : 1 + n] = packet[off : off + n]
        frames.append(bytes(cf))
        off += n
    return frames


def fc_frame(bs: int = FC_BS, stmin: int = FC_STMIN) -> bytes:
    f = bytearray(8)
    f[0] = 0x30
    f[1] = bs
    f[2] = stmin
    return bytes(f)


class Receiver:
    """Feed received frames (8 bytes). On FF and every BS consecutive CFs the
    receiver must send an FC frame back - pass send_fc=callable to emit them
    automatically. .packet holds the completed packet (None otherwise);
    .last_fc holds the last FC received from the peer."""

    def __init__(self, send_fc=None):
        self.send_fc = send_fc
        self.reset()

    def reset(self):
        self.buf = bytearray()
        self.expected = 0
        self.last_sn = 0
        self.cf_since_fc = 0
        self.packet = None
        self.last_fc = None

    def feed(self, frame: bytes):
        pci = frame[0] >> 4
        if pci == 0x0:
            n = frame[0] & 0x0F
            if 1 <= n <= 7:
                self.packet = bytes(frame[1 : 1 + n])
            return
        if pci == 0x1:
            self.expected = ((frame[0] & 0x0F) << 8) | frame[1]
            self.buf = bytearray(frame[2:8])
            self.last_sn = 0
            self.cf_since_fc = 0
            self._emit_fc()
            return
        if pci == 0x2:
            sn = frame[0] & 0x0F
            if self.expected == 0 or sn != ((self.last_sn + 1) & 0x0F):
                self.reset()  # lost/duplicated frame: abort (MCU does the same)
                return
            self.last_sn = sn
            need = self.expected - len(self.buf)
            self.buf += frame[1 : 1 + min(7, need)]
            self.cf_since_fc += 1
            if len(self.buf) < self.expected and self.cf_since_fc >= FC_BS:
                self._emit_fc()
                self.cf_since_fc = 0
            if len(self.buf) >= self.expected:
                self.packet = bytes(self.buf)
            return
        if pci == 0x3:
            if frame[0] == 0x30:
                stmin = frame[2] if frame[2] <= 0xF0 else 1
                self.last_fc = (frame[1], stmin)
            else:
                self.last_fc = None  # OVFLW / wait / abort
            return

    def _emit_fc(self):
        if self.send_fc:
            self.send_fc(fc_frame())
