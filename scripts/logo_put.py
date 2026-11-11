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
  logo_put.py upload image.png [--port PORT] [--fit contain|cover|stretch] [--hex]
  (PORT defaults to the one /dev/cu.usbmodem* or /dev/ttyACM* present)
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
CHUNK = 48          # bytes per legacy "logo data" line: 16 + 96 chars < the 127-char shell line
BLOCK = 1024        # bytes per "logo blk" (firmware LOGO_BLK_MAX)
RETRIES = 8         # consecutive failed blocks before giving up
DEVICE_IDLE_S = 2.0 # firmware RAW_BLOCK_IDLE_MS: a half block is dropped after this
READY_TIMEOUT_S = 3.0
ACK_TIMEOUT_S = 5.0 # 1 KB over USB + 4 W25Q page programs: ~5-10 ms normally


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


def block_plan(payload, block=BLOCK):
    """(offset, bytes, crc32) of every 'logo blk' block, in order."""
    if len(payload) != PIXEL_BYTES:
        raise ValueError("payload is %d bytes, need %d" % (len(payload), PIXEL_BYTES))
    return [(off, payload[off:off + block], crc32(payload[off:off + block]))
            for off in range(0, len(payload), block)]


class DeviceTimeout(Exception):
    pass


def default_port():
    import glob
    ports = sorted(glob.glob("/dev/cu.usbmodem*")) + sorted(glob.glob("/dev/ttyACM*"))
    if len(ports) != 1:
        raise SystemExit("cannot pick a port (found %s): pass --port" % (ports or "none"))
    return ports[0]


class Shell:
    def __init__(self, port):
        import serial
        self.s = serial.Serial(port, 115200, timeout=0.05)
        self.s.write(b"\r\n")
        time.sleep(0.2)
        self.s.reset_input_buffer()
        self.buf = b""

    def wait(self, timeout):
        """The next complete reply line that starts with 'logo:'."""
        t0 = time.time()
        while True:
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                line = line.replace(b"\r", b"").lstrip(b"> ")
                if line.startswith(b"logo:"):
                    return line.decode(errors="replace")
            if time.time() - t0 > timeout:
                raise DeviceTimeout("no 'logo:' reply within %.1f s" % timeout)
            self.buf += self.s.read(512)

    def send(self, line):
        self.s.write(line.encode() + b"\r\n")

    def cmd(self, line, timeout=10.0):
        """Send one line, return its 'logo:' reply line."""
        self.buf = b""
        self.s.reset_input_buffer()
        self.send(line)
        try:
            return self.wait(timeout)
        except DeviceTimeout:
            raise SystemExit("no reply to %r within %.0f s" % (line[:40], timeout))

    def resync(self):
        """After a lost reply: let the device drop any half block, then ask
        where the upload stands. Returns (received, active). Late replies to
        the failed block (an "ok" that was only slow) are skipped."""
        time.sleep(DEVICE_IDLE_S + 0.5)
        self.s.reset_input_buffer()
        self.buf = b""
        self.send("")                      # terminate whatever line was half-typed
        self.send("logo pos")
        t0 = time.time()
        while time.time() - t0 < 5.0:
            try:
                r = self.wait(5.0)
            except DeviceTimeout:
                break
            parts = r.split()
            if len(parts) >= 4 and parts[1] == "pos":
                return int(parts[2]), parts[3] == "active"
        raise SystemExit("no reply to 'logo pos': device not answering")


def expect_offset(reply):
    """'... (expect N)' -> N, else None."""
    if "expect " in reply:
        try:
            return int(reply.rsplit("expect ", 1)[1].rstrip(")"))
        except ValueError:
            return None
    return None


def upload_blocks(sh, payload, block=BLOCK, log=print):
    plan = {off: (data, c) for off, data, c in block_plan(payload, block)}
    off, fails, done_bytes = 0, 0, 0
    t0 = time.time()
    last_pct = -1
    while off < len(payload):
        data, c = plan.get(off, (None, None))
        if data is None:                   # resynced into the middle of a block
            data = payload[off:off + block]
            c = crc32(data)
        try:
            sh.buf = b""
            # "\r" only: the block's first byte could be 0x0A, so the header
            # must not end in a "\n" that might reach the device in a later
            # USB packet than the "\r" (the firmware only skips a "\n" that
            # shares the packet).
            sh.buf = b""
            sh.s.write(("logo blk %d %d %08X\r" % (off, len(data), c)).encode())
            r = sh.wait(READY_TIMEOUT_S)
            if r == "logo: ready":
                sh.s.write(data)
                r = sh.wait(ACK_TIMEOUT_S)
            if r.startswith("logo: ok "):
                off = int(r.split()[2])
                fails = 0
                pct = 100 * off // len(payload)
                if pct // 10 != last_pct // 10:
                    log("  %3d%%  %.1f s" % (pct, time.time() - t0))
                    last_pct = pct
                continue
            nxt = expect_offset(r)
            if r.startswith("logo: err sequence") and nxt is not None:
                off = nxt                  # an earlier block's ack was lost; it did land
                continue
            raise DeviceTimeout(r)
        except DeviceTimeout as e:
            fails += 1
            log("  block @%d: %s; retry %d/%d" % (off, e, fails, RETRIES))
            if fails > RETRIES:
                raise SystemExit("upload failed at %d bytes after %d retries; slot left empty" % (off, RETRIES))
            pos, active = sh.resync()
            if not active:
                raise SystemExit("device lost the upload session (logo pos: idle at %d); start again" % pos)
            off = pos
    return time.time() - t0


def upload(port, payload, hex_lines=False):
    sh = Shell(port)
    t0 = time.time()
    if hex_lines:                          # firmware without `logo blk`
        lines = list(command_lines(payload))
        for i, line in enumerate(lines):
            # put erases 38 sectors (seconds); end re-reads 150 KB and CRCs it
            r = sh.cmd(line, timeout=30.0 if i in (0, len(lines) - 1) else 5.0)
            if "err" in r:
                raise SystemExit("device refused %r: %s\nupload aborted, slot left empty" % (line[:30], r))
            if i % 400 == 0:
                print("  %3d%%" % (100 * i // len(lines)), flush=True)
    else:
        r = sh.cmd("logo put %d %08X" % (len(payload), crc32(payload)), timeout=60.0)
        if not r.startswith("logo: ok"):
            raise SystemExit("device refused 'logo put': %s" % r)
        print("  erased in %.1f s" % (time.time() - t0), flush=True)
        dt = upload_blocks(sh, payload, log=lambda m: print(m, flush=True))
        print("  data in %.1f s (%.0f KB/s)" % (dt, len(payload) / 1024.0 / max(dt, 1e-3)), flush=True)
        r = sh.cmd("logo end", timeout=30.0)
        if not r.startswith("logo: ok"):
            raise SystemExit("device refused 'logo end': %s" % r)
    print("committed in %.1f s: %s" % (time.time() - t0, r))
    print(sh.cmd("logo info", timeout=10.0))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("action", choices=["upload", "convert", "show", "info", "clear"])
    ap.add_argument("files", nargs="*")
    ap.add_argument("--port", default=None, help="serial port (default: the only usbmodem/ttyACM)")
    ap.add_argument("--hex", action="store_true", help="legacy 48-byte hex lines (firmware without 'logo blk')")
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
        upload(a.port or default_port(), data, hex_lines=a.hex)
    else:
        print(Shell(a.port or default_port()).cmd("logo " + a.action, timeout=15.0))


if __name__ == "__main__":
    main()
