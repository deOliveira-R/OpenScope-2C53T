#!/usr/bin/env python3
"""Upload a boot logo to OpenScope over the USB CDC shell.

Converts a PNG/BMP/JPEG (any size: scaled to fit 320x240, centred on black) to
the stored format -- 320x240 RGB565 little-endian, row 0 first, 153,600 bytes
(firmware/src/util/logo_store.h) -- and sends it with

  logo put 153600 <crc32>      erase the slot
  logo data <off> <hex>        48 bytes per line, strictly sequential
  logo end                     the device re-reads flash, checks the CRC, commits

Every reply is checked; any "err" aborts, and a failed upload leaves the slot
empty (the built-in splash shows). The CRC is the standard CRC-32 (zlib).

Usage:
  logo_put.py upload image.png [--port /dev/ttyACM0] [--fit contain|cover|stretch]
  logo_put.py convert image.png out.rgb565      # no device: write the stored format
  logo_put.py upload --raw out.rgb565           # send an already-converted file
  logo_put.py show | info | clear [--port ...]
"""
import argparse
import sys
import time
import zlib

W, H = 320, 240
PIXEL_BYTES = W * H * 2
CHUNK = 48          # bytes per "logo data" line: 16 + 96 chars < the 127-char shell line


def rgb888_to_565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def rgb_bytes_to_rgb565_le(rgb):
    """Packed R,G,B bytes (W*H*3) -> little-endian RGB565 bytes (W*H*2)."""
    if len(rgb) % 3:
        raise ValueError("RGB data length is not a multiple of 3")
    out = bytearray(len(rgb) // 3 * 2)
    for i in range(len(rgb) // 3):
        v = rgb888_to_565(rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2])
        out[2 * i] = v & 0xFF
        out[2 * i + 1] = v >> 8
    return bytes(out)


def load_payload(path, fit="contain"):
    from PIL import Image
    img = Image.open(path).convert("RGB")
    if fit == "stretch":
        img = img.resize((W, H), Image.LANCZOS)
    else:
        sx, sy = W / img.width, H / img.height
        s = min(sx, sy) if fit == "contain" else max(sx, sy)
        nw, nh = max(1, round(img.width * s)), max(1, round(img.height * s))
        img = img.resize((nw, nh), Image.LANCZOS)
        canvas = Image.new("RGB", (W, H), (0, 0, 0))
        canvas.paste(img, ((W - nw) // 2, (H - nh) // 2))
        img = canvas
    return rgb_bytes_to_rgb565_le(img.tobytes())


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def command_lines(payload):
    """The exact shell lines of one upload, in order."""
    if len(payload) != PIXEL_BYTES:
        raise ValueError("payload is %d bytes, need %d" % (len(payload), PIXEL_BYTES))
    yield "logo put %d %08X" % (len(payload), crc32(payload))
    for off in range(0, len(payload), CHUNK):
        yield "logo data %d %s" % (off, payload[off:off + CHUNK].hex().upper())
    yield "logo end"


class Shell:
    def __init__(self, port):
        import serial
        self.s = serial.Serial(port, 115200, timeout=0.2)
        self.s.write(b"\r\n")
        time.sleep(0.2)
        self.s.reset_input_buffer()

    def cmd(self, line, timeout=10.0):
        """Send one line, return the first reply line that starts with 'logo:'."""
        self.s.write(line.encode() + b"\r\n")
        buf = b""
        t0 = time.time()
        while time.time() - t0 < timeout:
            buf += self.s.read(256)
            for l in buf.replace(b"\r", b"").split(b"\n"):
                if l.startswith(b"logo:"):
                    return l.decode(errors="replace")
        raise SystemExit("no reply to %r within %.0f s" % (line[:40], timeout))


def upload(port, payload):
    sh = Shell(port)
    t0 = time.time()
    lines = list(command_lines(payload))
    for i, line in enumerate(lines):
        # put erases 38 sectors (seconds); end re-reads 150 KB and CRCs it
        r = sh.cmd(line, timeout=30.0 if i in (0, len(lines) - 1) else 5.0)
        if "err" in r:
            raise SystemExit("device refused %r: %s\nupload aborted, slot left empty" % (line[:30], r))
        if i % 400 == 0:
            print("  %3d%%" % (100 * i // len(lines)), flush=True)
    print("committed in %.1f s: %s" % (time.time() - t0, r))
    print(sh.cmd("logo info", timeout=10.0))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("action", choices=["upload", "convert", "show", "info", "clear"])
    ap.add_argument("files", nargs="*")
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--fit", choices=["contain", "cover", "stretch"], default="contain")
    ap.add_argument("--raw", action="store_true", help="input is an already-converted .rgb565 file")
    a = ap.parse_args(argv)

    if a.action == "convert":
        if len(a.files) != 2:
            ap.error("convert needs: image out.rgb565")
        data = load_payload(a.files[0], a.fit)
        open(a.files[1], "wb").write(data)
        print("wrote %s: %d bytes, crc32 %08X" % (a.files[1], len(data), crc32(data)))
    elif a.action == "upload":
        if len(a.files) != 1:
            ap.error("upload needs one image file")
        data = open(a.files[0], "rb").read() if a.raw else load_payload(a.files[0], a.fit)
        upload(a.port, data)
    else:
        print(Shell(a.port).cmd("logo " + a.action, timeout=15.0))


if __name__ == "__main__":
    main()
