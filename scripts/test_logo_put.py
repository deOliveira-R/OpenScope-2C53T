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

    def test_wrong_size_refused(self):
        with self.assertRaises(ValueError):
            list(lp.command_lines(b"\0" * 10))


if __name__ == "__main__":
    unittest.main()
