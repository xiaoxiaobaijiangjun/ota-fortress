"""Sign a firmware image into an uploadable FOTA package (image + TLV trailer).

Usage:
  python tools/sign.py App/MDK-ARM/Objects/FOTA_App.bin --sv 1 \
      -o Host/demo_firmware/FOTA_App_signed.bin

Upload the OUTPUT file with the normal host flow, e.g.:
  python Host/fota_host.py --port COM5 --slot B update Host/demo_firmware/FOTA_App_signed.bin

The host tool splits image/trailer itself; ACTIVATE carries image_len = the
image part only, and the bootloader verifies CRC + digest + ECDSA signature.
"""

import argparse
import sys
from pathlib import Path

import ecdsa

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fota_tlv import build_package, compute_digest, verify_package

ROOT = Path(__file__).resolve().parent.parent


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("image", help="input firmware .bin (bare image)")
    ap.add_argument("--sv", type=int, default=1, help="security version counter (default 1)")
    ap.add_argument("--key", default=str(ROOT / "keys" / "fota_private.pem"), help="private key PEM")
    ap.add_argument("-o", "--out", required=True, help="output signed package path")
    args = ap.parse_args()

    image = Path(args.image).read_bytes()
    if len(image) < 8:
        sys.exit("image too small (< 8 bytes)")

    sk = ecdsa.SigningKey.from_pem(Path(args.key).read_bytes())
    package = build_package(image, args.sv, sk)
    Path(args.out).write_bytes(package)

    # Round-trip self-check against the canonical public key before shipping.
    pub = (ROOT / "tools" / "fota_public.pem").read_bytes()
    ok, reason, img, sv = verify_package(package, pub)
    if not ok or img != image or sv != args.sv:
        sys.exit(f"self-check FAILED: {reason}")
    print(f"signed package : {args.out}")
    print(f"image          : {len(image)} bytes (activate carries this length)")
    print(f"package        : {len(package)} bytes (trailer +{len(package) - len(image)})")
    print(f"security ver   : {args.sv}")
    print(f"sha256(img|sv) : {compute_digest(image, args.sv).hex()}")
    print(f"self-check     : {reason}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
