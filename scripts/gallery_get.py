#!/usr/bin/env python3
"""Fetch screenshots saved on the device (parity C16) over the USB shell.

The device stores each SAVE as a complete 4-bpp BMP in its W25Q scratch region
(docs/gallery.md). `screenshot get <n>` streams it framed as

    SHOTBIN name=<n>.bmp len=38518 crc32=XXXXXXXX\\r\\n <raw bytes> \\r\\nSHOTBIN END crc32=XXXXXXXX

The CRC recorded at save time and the CRC of what was sent must both match the
received bytes, or nothing is written.

Usage:
  gallery_get.py list            [--port /dev/cu.usbmodemXXXX]
  gallery_get.py get 3 [out.bmp] [--port ...]
  gallery_get.py all  outdir/    [--port ...]
"""
import argparse
import os
import re
import sys
import time
import zlib

import serial

HDR = re.compile(rb"SHOTBIN name=(\S+) len=(\d+) crc32=([0-9A-F]{8})\r\n")
END = re.compile(rb"\r\nSHOTBIN END crc32=([0-9A-F]{8})\r\n")
ROW = re.compile(rb"^\s+(\d+)\.bmp\s+slot", re.M)


def cmd(s, line, wait=1.0):
    s.reset_input_buffer()
    s.write(line.encode() + b"\r\n")
    buf, t0 = b"", time.time()
    while time.time() - t0 < wait:
        buf += s.read(4096)
    return buf


def get(s, n, timeout=20.0):
    s.reset_input_buffer()
    s.write(f"screenshot get {n}\r\n".encode())
    buf, t0 = b"", time.time()
    while time.time() - t0 < timeout:
        buf += s.read(4096)
        m = HDR.search(buf)
        if m:
            need = m.end() + int(m.group(2))
            e = END.search(buf, need) if len(buf) >= need else None
            if e:
                data = buf[m.end():need]
                c = zlib.crc32(data) & 0xFFFFFFFF
                stored, sent = int(m.group(3), 16), int(e.group(1), 16)
                if c != stored or c != sent:
                    raise SystemExit(f"CRC mismatch: got {c:08X}, stored {stored:08X}, sent {sent:08X}")
                return m.group(1).decode(), data
        if b"screenshot:" in buf and not m:
            raise SystemExit(buf.decode(errors="replace").strip())
    raise SystemExit("timeout waiting for SHOTBIN")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("action", choices=["list", "get", "all"])
    ap.add_argument("arg", nargs="?")
    ap.add_argument("out", nargs="?")
    ap.add_argument("--port", default=os.environ.get("OPENSCOPE_PORT", "/dev/ttyACM0"))
    a = ap.parse_args()
    with serial.Serial(a.port, 115200, timeout=0.2) as s:
        if a.action == "list":
            sys.stdout.write(cmd(s, "screenshot list").decode(errors="replace"))
        elif a.action == "get":
            name, data = get(s, a.arg)
            out = a.out or name
            open(out, "wb").write(data)
            print(f"{out}: {len(data)} bytes, CRC OK")
        else:
            outdir = a.arg or "."
            os.makedirs(outdir, exist_ok=True)
            for n in ROW.findall(cmd(s, "screenshot list")):
                name, data = get(s, n.decode())
                open(os.path.join(outdir, name), "wb").write(data)
                print(f"{name}: {len(data)} bytes, CRC OK")


if __name__ == "__main__":
    main()
