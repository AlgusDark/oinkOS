#!/usr/bin/env python3
"""Upload a firmware .bin to the device's /firmware folder over Wi-Fi.

On the device, open File Transfer (join a network or create a hotspot) first.
Then install with Settings > System > SD Card Firmware Update.

Usage: scripts/upload_firmware.py [--host crosspoint.local] [--bin PATH] [--name NAME]
"""

import argparse
import json
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
FOLDER = "firmware"


def default_name():
    rev = subprocess.run(["git", "-C", str(REPO), "rev-parse", "--short", "HEAD"], capture_output=True, text=True)
    dirty = subprocess.run(["git", "-C", str(REPO), "diff", "--quiet", "HEAD", "--", "src", "lib"]).returncode != 0
    return f"oinkos-{rev.stdout.strip() or 'local'}{'-dirty' if dirty else ''}.bin"


def request(url, data=None, headers=None, timeout=30):
    req = urllib.request.Request(url, data=data, headers=headers or {}, method="POST" if data is not None else "GET")
    with urllib.request.urlopen(req, timeout=timeout) as reply:
        return reply.read().decode(errors="replace")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default="crosspoint.local", help="device address shown on the File Transfer screen")
    parser.add_argument("--bin", type=Path, default=REPO / ".pio/build/oinkos/firmware.bin")
    parser.add_argument("--name", help="file name on the card (default: oinkos-<git revision>[-dirty].bin)")
    args = parser.parse_args()

    if not args.bin.is_file():
        sys.exit(f"No firmware at {args.bin}. Build it with: direnv exec . pio run -e oinkos")
    data = args.bin.read_bytes()
    name = args.name or default_name()
    base = args.host if args.host.startswith("http") else f"http://{args.host}"

    try:
        request(f"{base}/mkdir", urllib.parse.urlencode({"name": FOLDER, "path": "/"}).encode(),
                {"Content-Type": "application/x-www-form-urlencoded"})
    except urllib.error.HTTPError as e:
        if e.code != 400:  # 400 means the folder already exists
            raise

    boundary = "oinkos-firmware-upload"
    body = (f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="{name}"\r\n'
            "Content-Type: application/octet-stream\r\n\r\n").encode() + data + f"\r\n--{boundary}--\r\n".encode()
    print(f"Uploading {args.bin} ({len(data)} bytes) to {base}/{FOLDER}/{name} ...")
    reply = request(f"{base}/upload?path=/{FOLDER}&overwrite=1", body,
                    {"Content-Type": f"multipart/form-data; boundary={boundary}"}, timeout=600)

    listing = json.loads(request(f"{base}/api/files?path=/{FOLDER}"))
    stored = next((f["size"] for f in listing if f["name"] == name), None)
    if stored != len(data):
        sys.exit(f"Upload check failed: card has {stored} bytes, expected {len(data)}. Server said: {reply.strip()}")
    print(f"OK: /{FOLDER}/{name} is on the card ({stored} bytes).")
    print(f"On the device: Settings > System > SD Card Firmware Update, open the {FOLDER} folder, then pick that file.")


if __name__ == "__main__":
    try:
        main()
    except urllib.error.HTTPError as e:  # a URLError subclass: the device answered
        sys.exit(f"The device rejected the request: HTTP {e.code} {e.reason}\n"
                 f"{e.read().decode(errors='replace').strip()}")
    except urllib.error.URLError as e:
        sys.exit(f"Cannot reach the device: {e.reason}\n"
                 "Check that File Transfer is open, then pass the address it shows with --host.\n"
                 "On macOS, 'No route to host' usually means this terminal app lacks Local Network access\n"
                 "(System Settings > Privacy & Security > Local Network).")
