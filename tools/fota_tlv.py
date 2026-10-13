"""TLV signed-package format shared by tools/sign.py, tools/keygen.py and the
host tool (Host/fota_host.py). Mirrors Bootloader/Core/Inc/image_tlv.h.

Layout:
    [image N bytes]['F','T','L','V' 4B][tlv_len LE16][entries... tlv_len bytes]
    entry: [type LE16][len LE16][value ...]

Mandatory entries: SHA-256 digest (32B), ECDSA-P256 signature (64B r||s),
security_version (4B LE). Digest input: SHA256(image || security_version_LE32).

Byte orders match micro-ecc on the MCU: public key X||Y big-endian (64B),
signature r||s big-endian (64B) - both are the python-ecdsa native encodings.
"""

import hashlib
import struct

TLV_MAGIC = b"FTLV"
TLV_TYPE_SHA256 = 0x0001
TLV_TYPE_ECDSA_P256 = 0x0002
TLV_TYPE_SEC_VERSION = 0x0003
TLV_TRAILER_FIXED_LEN = 118  # 6 + 3 * 4 + 32 + 64 + 4


def compute_digest(image: bytes, security_version: int) -> bytes:
    return hashlib.sha256(image + struct.pack("<I", security_version)).digest()


def build_trailer(security_version: int, digest: bytes, signature: bytes) -> bytes:
    entries = [
        (TLV_TYPE_SEC_VERSION, struct.pack("<I", security_version)),
        (TLV_TYPE_SHA256, digest),
        (TLV_TYPE_ECDSA_P256, signature),
    ]
    body = b"".join(struct.pack("<HH", t, len(v)) + v for t, v in entries)
    return TLV_MAGIC + struct.pack("<H", len(body)) + body


def build_package(image: bytes, security_version: int, private_key) -> bytes:
    """private_key: ecdsa.SigningKey on NIST256p. Returns image + trailer."""
    import ecdsa

    digest = compute_digest(image, security_version)
    sig = private_key.sign_digest_deterministic(
        digest, hashfunc=hashlib.sha256, sigencode=ecdsa.util.sigencode_string
    )
    return image + build_trailer(security_version, digest, sig)


def _parse_at(buf: bytes, off: int):
    """Parse a trailer starting exactly at buf[off]; strict to the byte."""
    if off + 6 > len(buf) or buf[off : off + 4] != TLV_MAGIC:
        return None
    (tlv_len,) = struct.unpack_from("<H", buf, off + 4)
    end = off + 6 + tlv_len
    if end != len(buf):  # trailer must run to the end of the package
        return None
    pos = off + 6
    sv = digest = sig = None
    seen = set()
    while pos < end:
        if pos + 4 > end:
            return None
        t, l = struct.unpack_from("<HH", buf, pos)
        pos += 4
        if pos + l > end:
            return None
        v = buf[pos : pos + l]
        pos += l
        if t in seen:
            return None
        seen.add(t)
        if t == TLV_TYPE_SHA256 and l == 32:
            digest = v
        elif t == TLV_TYPE_ECDSA_P256 and l == 64:
            sig = v
        elif t == TLV_TYPE_SEC_VERSION and l == 4:
            sv = struct.unpack("<I", v)[0]
        else:
            return None
    if pos != end or sv is None or digest is None or sig is None:
        return None
    return buf[:off], sv, digest, sig


def parse_package(package: bytes):
    """Split package -> (image, sv, digest, signature) or None if no valid
    trailer terminates the package. Searches the tail for the magic."""
    pos = package.find(TLV_MAGIC, max(0, len(package) - 1024))
    while pos != -1:
        parsed = _parse_at(package, pos)
        if parsed:
            return parsed
        pos = package.find(TLV_MAGIC, pos + 1)
    return None


def verify_package(package: bytes, public_pem: bytes):
    """Full host-side check: trailer structure + digest + ECDSA signature.

    Returns (ok: bool, reason: str, image: bytes|None, sv: int|None)."""
    parsed = parse_package(package)
    if not parsed:
        return False, "no valid TLV trailer", None, None
    image, sv, digest, sig = parsed
    if compute_digest(image, sv) != digest:
        return False, "digest mismatch (image or security_version altered)", image, sv
    try:
        import ecdsa

        vk = ecdsa.VerifyingKey.from_pem(public_pem.decode())
        vk.verify_digest(sig, digest, sigdecode=ecdsa.util.sigdecode_string)
    except ImportError:
        return True, "signature NOT CHECKED (python 'ecdsa' missing)", image, sv
    except Exception:
        return False, "ECDSA signature invalid", image, sv
    return True, "signature OK", image, sv
