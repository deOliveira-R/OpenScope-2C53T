#!/usr/bin/env python3
"""Host test for scripts/fpga_put.py: .fs packing and the upload command lines (synthetic data)."""
import os
import sys
import tempfile
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fpga_put


def main():
    data = bytes((i * 37 + 5) & 0xFF for i in range(1000))
    with tempfile.TemporaryDirectory() as d:
        fs = os.path.join(d, "x.fs")
        with open(fs, "w") as f:
            f.write("//comment line\n")
            bits = "".join(format(b, "08b") for b in data)
            for i in range(0, len(bits), 64):
                f.write(bits[i:i + 64] + "\n")
        assert fpga_put.load_stream(fs) == data, ".fs packing"
        raw = os.path.join(d, "x.bin")
        open(raw, "wb").write(data)
        assert fpga_put.load_stream(raw) == data, ".bin"
        bad = os.path.join(d, "bad.fs")
        open(bad, "w").write("0101\n")
        try:
            fpga_put.load_stream(bad)
            raise AssertionError("ragged .fs accepted")
        except SystemExit:
            pass
    lines = list(fpga_put.command_lines(data, "t"))
    assert lines[0] == "fpga slot put 1000 %08X t" % (zlib.crc32(data) & 0xFFFFFFFF), lines[0]
    assert lines[-1] == "fpga slot end"
    got = b""
    for l in lines[1:-1]:
        _, _, _, off, hx = l.split()
        assert int(off) == len(got), "offsets not sequential"
        assert len(hx) <= 2 * fpga_put.CHUNK
        got += bytes.fromhex(hx)
    assert got == data, "data lines do not rebuild the payload"
    assert max(len(l) for l in lines) < 128, "line too long for the shell"
    assert list(fpga_put.command_lines(data, "t", force=True))[0].endswith(" force")

    # A reply is everything up to the prompt. `fpga slot info` answers with two lines; the second
    # arrives in a later read and must not become the reply to the next command.
    class FakeSerial:
        """Hands out scripted bytes one piece per read(); a command written queues its reply."""
        def __init__(self, replies):
            self.replies, self.pending, self.sent = replies, [], []
        def write(self, b):
            line = b.decode().strip()
            if line:
                self.sent.append(line)
                self.pending += self.replies[line]
        def reset_input_buffer(self):
            pass                                  # what is still on the wire cannot be discarded
        def read(self, n):
            return self.pending.pop(0) if self.pending else b""
    fake = FakeSerial({
        "fpga slot info": [b"fpga slot info\r\nfpga slot: valid 'x' 10 B\r\n", b"",
                           b"fpga slot: FPGA was sent: slot 'x'\r\n> \r\n> "],
        "fpga slot put 3 0 n": [b"fpga slot put 3 0 n\r\nfpga slot: ok put 3\r\n> \r\n> "],
    })
    sh = fpga_put.Shell(None, stream=fake)
    assert sh.cmd("fpga slot info") == "fpga slot: valid 'x' 10 B"
    assert sh.cmd("fpga slot put 3 0 n") == "fpga slot: ok put 3", "a late line of the last reply was taken as this one"
    print("test_fpga_put: ok")


if __name__ == "__main__":
    main()
