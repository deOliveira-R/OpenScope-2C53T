"""Host checks for scripts/logo_put.py (no device, no Pillow needed): the stored
format and the shell protocol lines that firmware/tests/test_logo.c's header
layout and firmware/src/drivers/usb_debug.c cmd_logo() expect."""
import importlib.util
import unittest
import zlib
from pathlib import Path

spec = importlib.util.spec_from_file_location("logo_put", Path(__file__).with_name("logo_put.py"))
lp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lp)


lp.DEVICE_IDLE_S = 0.02
lp.READY_TIMEOUT_S = 0.2
lp.ACK_TIMEOUT_S = 0.2


class FakeDevice:
    """The firmware's logo blk / logo pos protocol (usb_debug.c cmd_logo,
    raw_block.c) behind a pyserial-like object, with injected faults keyed by
    block number: drop_ack (block written, reply lost), corrupt (one byte
    flipped in transit -> 'err crc'), stall (host bytes stop half way ->
    device idle timeout), drop_ready."""

    def __init__(self, faults):
        self.faults = dict(faults)
        self.out = b""
        self.line = b""
        self.raw = None            # [off, n, crc, bytearray, t_last]
        self.flash = bytearray()
        self.active = False
        self.blocks_ok = 0
        self.nblk = 0

    def _reply(self, text):
        self.out += text.encode() + b"\r\n"

    def _raw_done(self):
        off, n, crc, data, _ = self.raw
        self.raw = None
        if zlib.crc32(bytes(data)) & 0xFFFFFFFF != crc:
            self._reply("logo: err crc (block @%d; expect %d)" % (off, len(self.flash)))
            return
        self.flash += data
        self.blocks_ok += 1
        if self.faults.pop(("ack", self.nblk), None) is None:
            self._reply("logo: ok %d" % len(self.flash))

    def write(self, b):
        import time as _t
        i = 0
        while i < len(b):
            if self.raw is not None:
                off, n, crc, data, _ = self.raw
                take = b[i:i + (n - len(data))]
                if self.faults.get(("stall", self.nblk)):
                    take = take[: max(0, n // 2 - len(data))]
                    self.faults.pop(("stall", self.nblk))
                    data += take
                    self.raw[4] = _t.time()
                    return len(b)                      # the rest never arrives
                if self.faults.pop(("corrupt", self.nblk), None) and take:
                    take = bytes([take[0] ^ 0xFF]) + take[1:]
                data += take
                self.raw[4] = _t.time()
                i += len(take)
                if len(data) == n:
                    self._raw_done()
                continue
            c = b[i:i + 1]
            i += 1
            if c in (b"\r", b"\n"):
                self._line(self.line.decode(errors="replace"))
                self.line = b""
                # firmware shell_feed: a "\n" right behind the "\r" in the
                # same packet is the line ending, not block data
                if self.raw is not None and c == b"\r" and b[i:i + 1] == b"\n":
                    i += 1
            else:
                self.line += c
        return len(b)

    def _line(self, line):
        p = line.split()
        if self.raw is None and line and not line.startswith("logo"):
            return
        if p[:2] == ["logo", "pos"]:
            self._reply("logo: pos %d %s" % (len(self.flash), "active" if self.active else "idle"))
        elif p[:2] == ["logo", "blk"]:
            off, n, crc = int(p[2]), int(p[3]), int(p[4], 16)
            if off != len(self.flash):
                self._reply("logo: err sequence (expect %d)" % len(self.flash))
                return
            self.nblk = off // 1024
            f = self.faults.pop(self.nblk, None)
            if f:
                self.faults[(f.replace("drop_ack", "ack").replace("drop_ready", "ready"), self.nblk)] = True
            self.raw = [off, n, crc, bytearray(), 0.0]
            if self.faults.pop(("ready", self.nblk), None):
                self.raw = None                        # line lost: never armed
                return
            self._reply("logo: ready")

    def read(self, n):
        import time as _t
        if self.raw is not None and self.raw[4] and _t.time() - self.raw[4] > 0.01:
            off = self.raw[0]
            self.raw = None
            self._reply("logo: err timeout (block @%d; expect %d)" % (off, len(self.flash)))
        if not self.out:
            _t.sleep(0.001)
        r, self.out = self.out[:n], self.out[n:]
        return r

    def reset_input_buffer(self):
        self.out = b""


class LogoPut(unittest.TestCase):
    def test_rgb565_primaries_little_endian(self):
        out = lp.rgb_bytes_to_rgb565_le(bytes([255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255, 0, 0, 0]))
        self.assertEqual(out, bytes([0x00, 0xF8, 0xE0, 0x07, 0x1F, 0x00, 0xFF, 0xFF, 0x00, 0x00]))

    def test_geometry(self):
        self.assertEqual(lp.PIXEL_BYTES, 153600)

    def test_protocol_lines(self):
        payload = bytes((i * 7) & 0xFF for i in range(lp.PIXEL_BYTES))
        lines = list(lp.command_lines(payload))
        self.assertEqual(lines[0], "logo put 153600 %08X" % (zlib.crc32(payload) & 0xFFFFFFFF))
        self.assertEqual(lines[-1], "logo end")
        data = lines[1:-1]
        self.assertEqual(len(data), lp.PIXEL_BYTES // lp.CHUNK)
        rebuilt = bytearray()
        for n, l in enumerate(data):
            _, _, off, hexs = l.split(" ")
            self.assertEqual(int(off), n * lp.CHUNK)          # strictly sequential
            self.assertLess(len(l) + 1, 128)                  # firmware CMD_BUF_SIZE 128 incl. NUL
            rebuilt += bytes.fromhex(hexs)
        self.assertEqual(bytes(rebuilt), payload)

    def test_block_plan(self):
        payload = bytes((i * 13) & 0xFF for i in range(lp.PIXEL_BYTES))
        plan = lp.block_plan(payload)
        self.assertEqual(len(plan), 150)
        self.assertEqual(b"".join(d for _, d, _ in plan), payload)
        for off, d, c in plan:
            self.assertEqual(c, zlib.crc32(d) & 0xFFFFFFFF)
            self.assertLessEqual(len(d), 1024)       # firmware LOGO_BLK_MAX
            # the header line fits the firmware's 127-character shell line
            self.assertLess(len("logo blk %d %d %08X" % (off, len(d), c)) + 1, 128)

    def _upload_through(self, faults):
        payload = bytes((i * 31 + 5) & 0xFF for i in range(lp.PIXEL_BYTES))
        dev = FakeDevice(faults)
        dev.active = True
        sh = lp.Shell.__new__(lp.Shell)
        sh.s, sh.buf = dev, b""
        logs = []
        lp.upload_blocks(sh, payload, log=logs.append)
        self.assertEqual(bytes(dev.flash), payload)
        return dev, logs

    def test_blocks_clean(self):
        dev, logs = self._upload_through({})
        self.assertEqual(dev.blocks_ok, 150)
        self.assertFalse([l for l in logs if "retry" in l])

    def test_blocks_survive_faults(self):
        # lost ack after the block landed, a corrupted block, a stalled half
        # block, a lost "ready": all recovered without losing or doubling data
        dev, logs = self._upload_through({5: "drop_ack", 17: "corrupt", 40: "stall", 90: "drop_ready"})
        self.assertEqual(len([l for l in logs if "retry" in l]), 4)

    def test_wrong_size_refused(self):
        with self.assertRaises(ValueError):
            list(lp.command_lines(b"\0" * 10))


if __name__ == "__main__":
    unittest.main()
