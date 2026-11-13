#!/usr/bin/env python3
"""Write an Android update manifest and a detached P-256 signature."""

import argparse
import hashlib
import json
from pathlib import Path
from urllib.parse import urlsplit

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature, encode_dss_signature


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=int, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--apk", type=Path, required=True)
    parser.add_argument("--url", required=True, help="HTTPS URL serving the APK")
    parser.add_argument("--key", type=Path, required=True, help="Android maintainer's P-256 PEM key")
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()

    if not 0 < args.build <= 0xFFFFFFFF:
        parser.error("--build must be a positive 32-bit build number")
    url = urlsplit(args.url)
    if url.scheme != "https" or not url.hostname or url.username or url.password:
        parser.error("--url must be an HTTPS download URL")
    if not args.apk.is_file() or args.apk.suffix.lower() != ".apk":
        parser.error("--apk must point to the release APK")

    key = serialization.load_pem_private_key(args.key.read_bytes(), password=None)
    if not isinstance(key, ec.EllipticCurvePrivateKey) or not isinstance(key.curve, ec.SECP256R1):
        parser.error("--key must be a P-256 EC private key")

    with args.apk.open("rb") as apk:
        digest = hashlib.file_digest(apk, "sha256").hexdigest()
    manifest = {
        "build": args.build,
        "version": args.version,
        "apk": args.apk.name,
        "url": args.url,
        "sha256": digest,
        "size": args.apk.stat().st_size,
    }
    payload = json.dumps(manifest, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    algorithm = ec.ECDSA(hashes.SHA256())
    r, s = decode_dss_signature(key.sign(payload, algorithm))
    key.public_key().verify(encode_dss_signature(r, s), payload, algorithm)

    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "latest-android.json").write_bytes(payload)
    (args.output_dir / "latest-android.json.sig").write_text(f"{r:064x}{s:064x}\n", encoding="ascii")


if __name__ == "__main__":
    main()
