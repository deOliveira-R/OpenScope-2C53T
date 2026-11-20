#!/usr/bin/env python3
"""Send an FPGA design to the scope's flash slot and, if asked, load it.

    python3 scripts/fpga_put.py DESIGN.fs|DESIGN.bin [--name NAME] [--port PORT] [--load] [--boot] [--force]

The firmware side is `fpga slot ...` and `fpga reload ...` (firmware/src/util/fpga_slot.h,
fpga_reload() in fpga.c). The design is a raw Gowin configuration stream: a `.fs`
file (ASCII bit lines, '//' comments ignored, packed MSB first) or the same bytes
as `.bin`.

  fpga slot put <len> <crc32hex> <name>   erase the slot, start an upload
  fpga slot data <off> <hex>              48 bytes per line at the next offset
  fpga slot end                           the device re-reads the flash, checks the CRC, commits

A new upload always clears the slot's boot flag; --boot sets it again, so the
design also loads at power-on (`fpga slot boot off` returns to the compiled-in one).

--load then sends `fpga reload slot`: the firmware power-cycles the FPGA (PC6) and
uploads the stored design. That only works when the design now in the FPGA lets it
power down (inputs without clamp diodes); otherwise the device says so and nothing
is changed. The CRC is the standard CRC-32 (zlib).
"""
import argparse
import sys
import time
import zlib

CHUNK = 48
PREFIXES = (b"fpga slot:", b"fpga reload:")


def load_stream(path):
    """Bytes of a .fs (bit lines) or .bin (raw) configuration stream."""
    if path.lower().endswith(".fs"):
        bits = "".join(l.strip() for l in open(path) if l.strip() and not l.startswith("//"))
        if set(bits) - {"0", "1"} or len(bits) % 8:
            raise SystemExit("%s: not a byte-aligned Gowin .fs bit file" % path)
        return bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8))
    return open(path, "rb").read()


def command_lines(payload, name, force=False):
    """Every line of an upload, in order."""
    yield "fpga slot put %d %08X %s%s" % (len(payload), zlib.crc32(payload) & 0xFFFFFFFF, name,
                                           " force" if force else "")
    for off in range(0, len(payload), CHUNK):
        yield "fpga slot data %d %s" % (off, payload[off:off + CHUNK].hex().upper())
    yield "fpga slot end"


def default_port():
    import glob
    ports = sorted(glob.glob("/dev/cu.usbmodem*")) + sorted(glob.glob("/dev/ttyACM*"))
    if len(ports) != 1:
        raise SystemExit("cannot pick a port (found %s): pass --port" % (ports or "none"))
    return ports[0]


class Shell:
    def __init__(self, port, stream=None):
        """port: a serial device; stream: an open one in its place (the host test's fake)."""
        if stream is None:
            import serial
            stream = serial.Serial(port, 115200, timeout=0.02)
        self.s = stream
        self.s.write(b"\r\n")
        time.sleep(0.2)
        self.s.reset_input_buffer()
        self.buf = b""

    def cmd(self, line, timeout=10.0):
        """Send one line and read its whole reply, up to the prompt line the shell prints after
        every command; return the reply's first line that starts with a known prefix.

        Reading to the prompt is what ties a reply to its command. A reply can have more than one
        line (`fpga slot info` has two) and its lines can arrive in separate reads: returning at
        the first line left the second to be taken as the next command's reply. That turned an
        accepted `fpga slot put` into a reported refusal after the device had erased the slot."""
        self.buf = b""
        self.s.reset_input_buffer()
        self.s.write(line.encode() + b"\r\n")
        t0 = time.time()
        first = None
        while True:
            while b"\n" in self.buf:
                reply, self.buf = self.buf.split(b"\n", 1)
                reply = reply.replace(b"\r", b"")
                if reply.strip() == b">":               # the prompt: the reply is complete
                    if first is not None:
                        return first
                    continue                            # a prompt left over from before this command
                reply = reply.lstrip(b"> ")
                if first is None and reply.startswith(PREFIXES):
                    first = reply.decode(errors="replace")
            if time.time() - t0 > timeout:
                raise SystemExit("no complete reply to %r within %.0f s%s" %
                                 (line[:40], timeout, "" if first is None else " (got %r, no prompt after it)" % first))
            self.buf += self.s.read(512)


def upload(sh, payload, name, force=False, log=print):
    t0 = time.time()
    lines = list(command_lines(payload, name, force))
    for i, line in enumerate(lines):
        r = sh.cmd(line, timeout=60.0 if i == 0 or i == len(lines) - 1 else 5.0)
        if " ok" not in r:
            raise SystemExit("device refused %r: %s" % (line[:48], r))
        if i and i % 400 == 0:
            log("  %d of %d bytes" % (min(i * CHUNK, len(payload)), len(payload)))
    log("stored %d bytes as '%s' in %.1f s: %s" % (len(payload), name, time.time() - t0, r))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("design")
    ap.add_argument("--name")
    ap.add_argument("--port")
    ap.add_argument("--load", action="store_true", help="then `fpga reload slot`")
    ap.add_argument("--force", action="store_true", help="erase a slot range that holds unknown data")
    ap.add_argument("--boot", action="store_true",
                    help="also set the boot flag (a new upload always clears it): the design loads at power-on")
    a = ap.parse_args(argv)
    payload = load_stream(a.design)
    name = (a.name or a.design.rsplit("/", 1)[-1].rsplit(".", 1)[0]).replace(" ", "_")[:40]
    sh = Shell(a.port or default_port())
    print(sh.cmd("fpga slot info"))
    upload(sh, payload, name, a.force)
    if a.boot:
        r = sh.cmd("fpga slot boot on")
        print(r)
        if " ok" not in r:
            return 1
    if a.load:
        r = sh.cmd("fpga reload slot", timeout=60.0)
        print(r)
        if " ok" not in r:
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
