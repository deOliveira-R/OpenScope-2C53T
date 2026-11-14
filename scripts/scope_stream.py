#!/usr/bin/env python3
"""Host side of the OpenScope binary stream (docs/stream_protocol.md).

The device sends binary frames on the same CDC port as the text shell. This
script separates the two (demux), decodes the frames, and:

  --stats N       run N seconds: frames/s per type, bytes/s, sequence gaps,
                  CRC errors, torn and missed records, the firmware's own
                  counters, and whether the shell answered `version` sent
                  half-way through (with its latency)
  --dump FILE     also write every received byte (text and frames, exactly
                  as received) to FILE; --replay reads it back
  --plot          live window: CH1 / CH2 in volts against seconds from the
                  trigger, grid at the scope's V/div and time/div, frame
                  rate in the title (matplotlib, imported only here)
  --replay FILE   run --stats (default) or --plot from a dump, no device
  --selftest      demultiplexer and decoders against the C encoder's
                  vectors (firmware/tests/fixtures/stream/), no device

  python3 scripts/scope_stream.py --port /dev/cu.usbmodemXXXX --stats 20
  python3 scripts/scope_stream.py --port /dev/cu.usbmodemXXXX --plot --dump cap.bin
  python3 scripts/scope_stream.py --replay cap.bin --plot

pyserial is needed only to talk to a device; it is imported lazily.
"""
import argparse
import binascii
import os
import random
import struct
import sys
import time
import zlib

SOF0, SOF1 = 0xFB, 0x53
HDR_LEN, CRC_LEN = 10, 4
MAX_PAYLOAD = 4096

T_HELLO, T_PROPS, T_RECORD, T_METER, T_STATS, T_PROPDESC = 1, 2, 3, 4, 5, 6
TYPE_NAMES = {1: "HELLO", 2: "PROPS", 3: "RECORD", 4: "METER", 5: "STATS", 6: "PROPDESC",
              7: "MEAS", 8: "GEN", 9: "CANFRAME", 10: "ROLL"}

REC_INTERLEAVED, REC_TIME_ORDERED, REC_ROLL = 1, 2, 4
REC_COHERENT, REC_RUNNING, REC_TRUE_SCALE = 8, 16, 32
TRIG_NONE = 0xFFFF


def crc16(data):
    """CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF)."""
    return binascii.crc_hqx(bytes(data), 0xFFFF)


def crc32(data):
    return zlib.crc32(bytes(data)) & 0xFFFFFFFF


# ── demultiplexer (pure) ────────────────────────────────────────────────────
def demux(buf, final=False):
    """Split bytes into text and frames.

    Returns (events, rest). events is a list of
      ("text", bytes)                       shell text (NUL padding removed)
      ("frame", type, seq, flags, payload)  a frame whose CRCs check
      ("bad", reason, offset)               a header that checked, a payload that did not
    rest is the tail that may be the start of a frame: prepend it to the next
    read. With final=True nothing is held back.

    Rules: a frame starts with 0xFB 0x53 and a header whose CRC-16 checks. On a
    payload CRC failure the header is dropped and scanning resumes right after
    it, so a frame hidden inside a truncated one is still found and no byte is
    skipped by a length that cannot be trusted. A 0xFB that does not start a
    valid header is text. NUL outside frames is padding.
    """
    buf = bytes(buf)
    events = []
    n = len(buf)
    i = 0           # scan position
    t0 = 0          # start of pending text

    def text(a, b):
        if b > a:
            chunk = buf[a:b].replace(b"\x00", b"")
            if chunk:
                if events and events[-1][0] == "text":
                    events[-1] = ("text", events[-1][1] + chunk)
                else:
                    events.append(("text", chunk))

    while True:
        j = buf.find(bytes([SOF0]), i)
        if j < 0:
            text(t0, n)
            return events, b""
        if j + HDR_LEN > n:
            if final:
                text(t0, n)
                return events, b""
            text(t0, j)
            return events, buf[j:]
        h = buf[j:j + HDR_LEN]
        ln = h[6] | (h[7] << 8)
        if h[1] != SOF1 or crc16(h[:8]) != (h[8] | (h[9] << 8)) or ln > MAX_PAYLOAD:
            i = j + 1
            continue
        total = HDR_LEN + ln + CRC_LEN
        if j + total > n:
            if final:
                text(t0, j)
                events.append(("bad", "truncated", j))
                i = t0 = j + HDR_LEN
                continue
            text(t0, j)
            return events, buf[j:]
        body = buf[j + 2:j + HDR_LEN + ln]
        crc = struct.unpack_from("<I", buf, j + HDR_LEN + ln)[0]
        text(t0, j)
        if crc32(body) != crc:
            events.append(("bad", "crc", j))
            i = t0 = j + HDR_LEN
            continue
        events.append(("frame", h[2], h[4] | (h[5] << 8), h[3], buf[j + HDR_LEN:j + HDR_LEN + ln]))
        i = t0 = j + total


class Demuxer:
    """Stateful wrapper: feed reads, get events."""

    def __init__(self):
        self.rest = b""

    def feed(self, data, final=False):
        events, self.rest = demux(self.rest + bytes(data), final)
        return events


# ── decoders ───────────────────────────────────────────────────────────────
class Reader:
    def __init__(self, b):
        self.b, self.o = b, 0

    def take(self, fmt):
        v = struct.unpack_from("<" + fmt, self.b, self.o)
        self.o += struct.calcsize("<" + fmt)
        return v if len(v) > 1 else v[0]

    def raw(self, n):
        v = self.b[self.o:self.o + n]
        if len(v) != n:
            raise struct.error("short payload")
        self.o += n
        return v

    def str(self):
        return self.raw(self.take("B")).decode("latin-1")


def decode(ftype, payload):
    """Payload -> dict. Unknown types give {"type": t, "len": n} (skipped by length)."""
    r = Reader(payload)
    d = {"type": ftype}
    if ftype == T_HELLO:
        (d["proto_major"], d["proto_minor"], d["caps"], d["on"], d["max_payload"], d["rec_samples"],
         d["n_props"], d["table_crc"]) = r.take("BBBBHHHI")
        d["fw"] = r.str()
    elif ftype == T_PROPDESC:
        (d["id"], d["type_"], d["unit"], d["decimals"], d["group"], d["flags"], d["min"], d["max"],
         d["def"]) = r.take("HBBBBHiii")
        d["name"] = r.str()
        d["unit_sym"] = r.str()
    elif ftype == T_PROPS:
        d["table_crc"], ng = r.take("IB")
        d["n_groups"] = ng
        d["epochs"] = [r.take("I") for _ in range(ng)]
        n = r.take("H")
        d["n_props"] = n
        d["props"] = [r.take("Hi") for _ in range(n)]
    elif ftype == T_RECORD:
        (d["gen"], d["t_ms"], d["fs_hz"], d["s_per_div"], d["n"], d["trig_idx"], d["flags"], d["kind"],
         d["tb_code"], d["nch"]) = r.take("IIffHHBBBB")
        chans = []
        for _ in range(d["nch"]):
            c = {}
            (c["id"], c["vdiv_idx"], c["zero_code"], c["coupling"], c["position"], c["probe_x"],
             c["volts_per_count"], c["volts_per_div"]) = r.take("BBBBhHff")
            chans.append(c)
        for c in chans:
            c["codes"] = r.raw(d["n"])
        d["ch"] = chans
        d["gen_end"] = r.take("I")
        d["torn"] = d["gen_end"] != d["gen"]
    elif ftype == T_METER:
        (d["update_count"], d["t_ms"], d["value"], d["bcd"], d["func"], d["auto_fn"], d["result_class"],
         d["status_code"], d["decimal_pos"], d["range_index"], d["flags"]) = r.take("IIfiBBBBBBB")
        d["unit"] = r.str()
        d["display"] = r.str()
        d["range_label"] = r.str()
    elif ftype == T_STATS:
        d["t_ms"], d["bytes"], d["rec_missed"], d["rec_torn"], d["aborts"] = r.take("IIIII")
        nt = r.take("B")
        d["sent"], d["dropped"] = {}, {}
        for _ in range(nt):
            t, s, x = r.take("BII")
            d["sent"][t], d["dropped"][t] = s, x
    else:
        d["len"] = len(payload)
    return d


# ── statistics ──────────────────────────────────────────────────────────────
class Stats:
    def __init__(self):
        self.t0 = None
        self.t1 = None
        self.count = {}
        self.frame_bytes = 0
        self.total_bytes = 0
        self.text = bytearray()
        self.seq_gaps = 0
        self.last_seq = None
        self.crc_errors = 0
        self.truncated = 0
        self.decode_errors = 0
        self.torn = 0
        self.gen_missed = 0
        self.last_gen = None
        self.fw_stats = None
        self.hello = None
        self.props = {}
        self.names = {}
        self.records = 0
        self.rec_times = []
        self.dev_t = None          # (first, last) device t_ms seen

    def add_bytes(self, n, now):
        if self.t0 is None:
            self.t0 = now
        self.t1 = now
        self.total_bytes += n

    def event(self, ev, now=None):
        kind = ev[0]
        if kind == "text":
            self.text += ev[1]
            return None
        if kind == "bad":
            if ev[1] == "crc":
                self.crc_errors += 1
            else:
                self.truncated += 1
            return None
        _, ftype, seq, _flags, payload = ev
        self.frame_bytes += HDR_LEN + len(payload) + CRC_LEN
        if self.last_seq is not None and ftype != T_HELLO:
            self.seq_gaps += (seq - self.last_seq - 1) & 0xFFFF
        self.last_seq = seq
        self.count[ftype] = self.count.get(ftype, 0) + 1
        try:
            d = decode(ftype, payload)
        except struct.error:
            self.decode_errors += 1
            return None
        if "t_ms" in d:
            a, b = self.dev_t or (d["t_ms"], d["t_ms"])
            self.dev_t = (min(a, d["t_ms"]), max(b, d["t_ms"]))
        if ftype == T_HELLO:
            self.hello = d
            self.last_gen = None
        elif ftype == T_PROPDESC:
            self.names[d["id"]] = d
        elif ftype == T_PROPS:
            self.props = dict(d["props"])
        elif ftype == T_STATS:
            self.fw_stats = d
        elif ftype == T_RECORD:
            self.records += 1
            if now is not None:
                self.rec_times.append(now)
            if d["torn"]:
                self.torn += 1
            if self.last_gen is not None:
                steps = ((d["gen"] - self.last_gen) & 0xFFFFFFFF) // 2
                if steps > 1:
                    self.gen_missed += steps - 1
            self.last_gen = d["gen"]
        return d

    def report(self, out=sys.stdout, duration=None):
        dt = duration if duration else ((self.t1 - self.t0) if self.t0 is not None and self.t1 != self.t0 else 0)
        rate = (lambda n: n / dt) if dt > 0 else (lambda n: float("nan"))
        p = lambda *a: print(*a, file=out)
        if self.hello:
            h = self.hello
            p("HELLO proto=%d.%d fw=%r props=%d table_crc=%08x"
              % (h["proto_major"], h["proto_minor"], h["fw"], h["n_props"], h["table_crc"]))
        p("duration_s=%.2f bytes=%d bytes_per_s=%.0f frame_bytes_per_s=%.0f"
          % (dt, self.total_bytes, rate(self.total_bytes), rate(self.frame_bytes)))
        for t in sorted(self.count):
            p("  %-8s frames=%6d per_s=%7.2f" % (TYPE_NAMES.get(t, "0x%02x" % t), self.count[t], rate(self.count[t])))
        p("seq_gaps=%d crc_errors=%d truncated=%d decode_errors=%d" % (
            self.seq_gaps, self.crc_errors, self.truncated, self.decode_errors))
        p("records=%d torn=%d gen_missed=%d (records published but not received)" % (
            self.records, self.torn, self.gen_missed))
        if self.fw_stats:
            s = self.fw_stats
            p("firmware: bytes=%d rec_missed=%d rec_torn=%d aborts=%d" % (
                s["bytes"], s["rec_missed"], s["rec_torn"], s["aborts"]))
            p("firmware sent/dropped: " + " ".join(
                "%s=%d/%d" % (TYPE_NAMES.get(t, t), s["sent"][t], s["dropped"][t])
                for t in sorted(s["sent"]) if s["sent"][t] or s["dropped"][t]))


# ── plotting helpers (pure part, tested without matplotlib) ─────────────────
SCREEN_DIVS_X, SCREEN_DIVS_Y = 12, 8


def record_traces(rec):
    """RECORD dict -> per-channel dicts with x (s from trigger), y (V or codes),
    unit, volts/div, the y window (8 divisions around the screen centre) and
    the x grid step (time/div)."""
    n, fs = rec["n"], rec["fs_hz"]
    ref = rec["trig_idx"] if rec["trig_idx"] != TRIG_NONE else n // 2
    dt = 1.0 / fs if fs > 0 else 1.0
    x = [(i - ref) * dt for i in range(n)]
    spd = rec["s_per_div"] if rec["s_per_div"] > 0 else 25 * dt
    out = []
    for c in rec["ch"]:
        vpc, vpd, z = c["volts_per_count"], c["volts_per_div"], c["zero_code"]
        if vpc > 0 and vpd > 0:
            y = [(v - z) * vpc for v in c["codes"]]
            centre = (100 - z) * vpc          # screen centre code 100 (true scale)
            unit, step = "V", vpd
        else:
            y = list(c["codes"])
            centre, unit, step = 128.0, "code", 25.0
        out.append({"name": "CH%d" % (c["id"] + 1), "x": x, "y": y, "unit": unit, "ydiv": step,
                    "ylim": (centre - SCREEN_DIVS_Y / 2 * step, centre + SCREEN_DIVS_Y / 2 * step),
                    "xdiv": spd})
    return out


def time_scale(step):
    """(factor, unit) so that a time/div step reads as 1..999 of the unit."""
    for k, s in ((1, "s"), (1e-3, "ms"), (1e-6, "µs"), (1e-9, "ns")):
        if step >= k:
            return 1.0 / k, s
    return 1e9, "ns"


def eng(v, unit):
    a = abs(v)
    for k, s in ((1e9, "G"), (1e6, "M"), (1e3, "k"), (1, ""), (1e-3, "m"), (1e-6, "µ"), (1e-9, "n")):
        if a >= k or k == 1e-9:
            return "%g %s%s" % (round(v / k, 3), s, unit)
    return "%g %s" % (v, unit)


COLORS = {"CH1": "#2a78d6", "CH2": "#eb6834"}   # categorical slots 1, 2
SURFACE, INK, INK2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"


class Plot:
    """Live window: one panel per channel (each with its own V/div grid; no
    shared second y-axis), shared time axis with the time/div grid."""

    def __init__(self, backend=None):
        import matplotlib
        if backend:
            matplotlib.use(backend)
        import matplotlib.pyplot as plt
        from matplotlib.ticker import MultipleLocator
        self.plt, self.ML = plt, MultipleLocator
        self.fig = None
        self.axes = {}
        self.lines = {}
        self.layout = None
        self.times = []

    def _build(self, names):
        plt = self.plt
        if self.fig is None:
            plt.ion()
            self.fig = plt.figure(figsize=(10, 6), facecolor=SURFACE)
        self.fig.clf()
        self.axes, self.lines = {}, {}
        first = None
        for k, nm in enumerate(names):
            ax = self.fig.add_subplot(len(names), 1, k + 1, sharex=first)
            first = first or ax
            ax.set_facecolor(SURFACE)
            ax.grid(True, which="both", color=GRID, linewidth=0.8)
            ax.tick_params(colors=INK2, labelsize=8)
            for s in ax.spines.values():
                s.set_color(GRID)
            (ln,) = ax.plot([], [], color=COLORS.get(nm, INK), linewidth=1.5)
            self.axes[nm], self.lines[nm] = ax, ln
        self.layout = tuple(names)

    def update(self, rec, now):
        tr = record_traces(rec)
        names = [t["name"] for t in tr]
        if self.layout != tuple(names):
            self._build(names)
        self.times = [t for t in self.times if now - t < 2.0] + [now]
        fps = (len(self.times) - 1) / (self.times[-1] - self.times[0]) if len(self.times) > 1 else 0.0
        k, tu = time_scale(tr[0]["xdiv"])
        for t in tr:
            ax, ln = self.axes[t["name"]], self.lines[t["name"]]
            ln.set_data([v * k for v in t["x"]], t["y"])
            ax.set_ylim(*t["ylim"])
            ax.yaxis.set_major_locator(self.ML(t["ydiv"]))
            span_divs = (t["x"][-1] - t["x"][0]) / t["xdiv"] if t["xdiv"] > 0 else 0
            # one grid line per division; labels every 5 when the record is
            # much wider than the screen's 12 divisions
            ax.xaxis.set_minor_locator(self.ML(t["xdiv"] * k))
            ax.xaxis.set_major_locator(self.ML(t["xdiv"] * k * (5 if span_divs > 2 * SCREEN_DIVS_X else 1)))
            ax.tick_params(axis="x", labelbottom=(t is tr[-1]))
            ax.set_xlim(t["x"][0] * k, t["x"][-1] * k)
            ax.set_ylabel("%s  %s/div" % (t["name"], eng(t["ydiv"], t["unit"]) if t["unit"] == "V"
                                          else "%g code" % t["ydiv"]), color=INK, fontsize=9)
        last = self.axes[names[-1]]
        last.set_xlabel("time from trigger (%s), grid %s/div" % (tu, eng(tr[0]["xdiv"], "s")), color=INK2, fontsize=9)
        flags = []
        if rec["torn"]:
            flags.append("TORN")
        if rec["trig_idx"] == TRIG_NONE:
            flags.append("untriggered")
        if rec["flags"] & REC_INTERLEAVED:
            flags.append("interleaved")
        self.fig.suptitle("OpenScope stream  %.1f records/s  gen %d  fs %s %s"
                          % (fps, rec["gen"], eng(rec["fs_hz"], "Sa/s"), " ".join(flags)),
                          color=INK, fontsize=11)
        self.fig.canvas.draw_idle()
        self.fig.canvas.flush_events()

    def alive(self):
        return self.fig is None or self.plt.fignum_exists(self.fig.number)

    def pause(self):
        self.plt.pause(0.001)


# ── device / replay loops ───────────────────────────────────────────────────
def open_port(port):
    try:
        import serial
    except ImportError:
        sys.exit("pyserial is needed to talk to the device (pip install pyserial)")
    s = serial.Serial(port, 115200, timeout=0.02)
    s.reset_input_buffer()
    return s


def run_device(a):
    ser = open_port(a.port)
    dump = open(a.dump, "wb") if a.dump else None
    dm, st = Demuxer(), Stats()
    plot = Plot() if a.plot else None
    cmd = "stream on" + ("" if not a.what else " " + " ".join(a.what)) + (" fast" if a.fast else "")
    ser.write(b"\r\n" + cmd.encode() + b"\r\n")
    t_start = time.monotonic()
    duration = a.stats if a.stats else None
    ver_sent = ver_seen = None
    text_mark = 0
    try:
        while True:
            now = time.monotonic()
            el = now - t_start
            if duration is not None and el >= duration:
                break
            if duration is not None and ver_sent is None and el >= duration / 2:
                text_mark = len(st.text)
                ver_sent = time.monotonic()
                ser.write(b"version\r\n")
            data = ser.read(65536)
            if data:
                if dump:
                    dump.write(data)
                st.add_bytes(len(data), now)
                for ev in dm.feed(data):
                    d = st.event(ev, now)
                    if plot and d and d["type"] == T_RECORD:
                        plot.update(d, now)
            if ver_sent is not None and ver_seen is None and b"OpenScope 2C53T" in st.text[text_mark:]:
                ver_seen = time.monotonic()
            if plot:
                if not plot.alive():
                    break
                plot.pause()
    except KeyboardInterrupt:
        pass
    ser.write(b"stream off\r\n")
    t_end = time.monotonic() + 0.5
    while time.monotonic() < t_end:
        data = ser.read(65536)
        if data:
            if dump:
                dump.write(data)
            for ev in dm.feed(data):
                st.event(ev, None)
    for ev in dm.feed(b"", final=True):
        st.event(ev, None)
    if dump:
        dump.close()
    ser.close()
    st.report(duration=duration)
    if duration is not None:
        if ver_seen is not None:
            print("shell: version answered during streaming, %.0f ms after the command" % ((ver_seen - ver_sent) * 1e3))
        else:
            print("shell: NO version reply during streaming")
    tail = bytes(st.text[-400:]).decode("utf-8", "replace")
    print("--- last shell text ---\n" + tail.rstrip())


def run_replay(a):
    with open(a.replay, "rb") as f:
        data = f.read()
    dm, st = Demuxer(), Stats()
    plot = Plot("Agg" if a.png else None) if a.plot else None
    step = 4096
    for o in range(0, len(data), step):
        chunk = data[o:o + step]
        st.add_bytes(len(chunk), o)
        for ev in dm.feed(chunk, final=(o + step >= len(data))):
            d = st.event(ev, None)
            if plot and d and d["type"] == T_RECORD:
                plot.update(d, time.monotonic())
                if a.png:
                    plot.fig.savefig(a.png, facecolor=SURFACE)
                else:
                    plot.pause()
                    time.sleep(a.replay_delay)
    span = (st.dev_t[1] - st.dev_t[0]) / 1e3 if st.dev_t else 0
    st.report(duration=span or None)
    if plot and not a.png:
        plot.plt.ioff()
        plot.plt.show()


# ── self-test ───────────────────────────────────────────────────────────────
def fixture_dir():
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.join(here, "..", "firmware", "tests", "fixtures", "stream")


def parse_expect(path):
    """vectors_expect.txt (written by tests/test_stream.c from its inputs)."""
    items = []
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("TEXT "):
                items.append(("text", bytes.fromhex(line[5:])))
            elif line.startswith("FRAME "):
                kv = {}
                for tok in line[6:].split(" "):
                    k, v = tok.split("=", 1)
                    kv[k] = v
                items.append(("frame", kv))
    return items


def _num(s):
    try:
        return int(s)
    except ValueError:
        return float(s)


def check_frame(exp, ev, fail):
    """Compare a decoded frame with the C test's expected fields."""
    _, ftype, seq, _flags, payload = ev
    t = int(exp["type"])
    if ftype != t or seq != int(exp["seq"]):
        fail("frame type/seq %d/%d, expected %d/%s" % (ftype, seq, t, exp["seq"]))
        return
    d = decode(ftype, payload)
    flat = {}
    for k, v in d.items():
        if k == "ch":
            for i, c in enumerate(v):
                for ck, cv in c.items():
                    if ck == "codes":
                        flat["ch%d.sum" % i] = sum(cv)
                        flat["ch%d.first" % i] = cv[0]
                        flat["ch%d.last" % i] = cv[-1]
                    else:
                        flat["ch%d.%s" % (i, ck)] = cv
        elif k == "epochs":
            flat[k] = ",".join(str(e) for e in v)
        elif k == "props":
            flat[k] = ",".join("%d:%d" % pv for pv in v)
        elif k in ("sent", "dropped"):
            for tt, vv in v.items():
                flat["%s%d" % (k, tt)] = vv
        elif isinstance(v, str):
            flat[k] = v.encode("latin-1").hex()
        else:
            flat[k] = v
    for k, v in exp.items():
        if k in ("type", "seq"):
            continue
        if k not in flat:
            fail("type %d: field %s missing from the decode" % (t, k))
            continue
        got = flat[k]
        if isinstance(got, float):
            want = float(v)
            if abs(got - want) > 1e-6 * max(1.0, abs(want)):
                fail("type %d: %s=%r, expected %s" % (t, k, got, v))
        elif isinstance(got, bool):
            pass
        elif isinstance(got, int):
            if got != _num(v):
                fail("type %d: %s=%r, expected %s" % (t, k, got, v))
        elif str(got) != v:
            fail("type %d: %s=%r, expected %s" % (t, k, got, v))
    if t == T_RECORD:
        torn = int(exp["gen"]) != int(exp["gen_end"])
        if d["torn"] != torn:
            fail("record torn flag %r, expected %r" % (d["torn"], torn))


def collect(chunks):
    """Feed chunks through a Demuxer; return (texts between frames, frames, bads)."""
    dm = Demuxer()
    evs = []
    for k, c in enumerate(chunks):
        evs += dm.feed(c, final=(k == len(chunks) - 1))
    return evs


def shape(evs):
    """Merge adjacent text: the sequence of ("text", bytes) / ("frame", ...)."""
    out = []
    for e in evs:
        if e[0] == "text" and out and out[-1][0] == "text":
            out[-1] = ("text", out[-1][1] + e[1])
        else:
            out.append(e)
    return out


def selftest():
    fails = []
    fail = fails.append
    fx = fixture_dir()
    with open(os.path.join(fx, "vectors.bin"), "rb") as f:
        vec = f.read()
    exp = parse_expect(os.path.join(fx, "vectors_expect.txt"))
    n_frames = sum(1 for e in exp if e[0] == "frame")

    # 1. whole buffer: exact sequence of text and frames, fields from the C inputs
    got = shape(collect([vec]))
    if [g[0] for g in got] != [e[0] for e in exp]:
        fail("event kinds %s, expected %s" % ([g[0] for g in got], [e[0] for e in exp]))
    else:
        for g, e in zip(got, exp):
            if e[0] == "text":
                if g[1] != e[1]:
                    fail("text %r, expected %r" % (g[1], e[1]))
            else:
                check_frame(e[1], g, fail)
    ref = got

    # 2. split across reads, every size from 1 byte, and random splits
    for size in list(range(1, 80)) + [511, 4096]:
        g = shape(collect([vec[o:o + size] for o in range(0, len(vec), size)]))
        if g != ref:
            fail("split at %d bytes per read changes the result" % size)
            break
    rnd = random.Random(1)
    for _ in range(200):
        cuts = sorted(rnd.sample(range(1, len(vec)), rnd.randint(1, 40)))
        parts = [vec[a:b] for a, b in zip([0] + cuts, cuts + [len(vec)])]
        if shape(collect(parts)) != ref:
            fail("random split %s changes the result" % cuts[:5])
            break

    frames = [e for e in ref if e[0] == "frame"]
    # locate the frames in the buffer (the demux reports no offsets for good ones)
    offs = []
    o = 0
    for fr in frames:
        o = vec.find(bytes([SOF0, SOF1, fr[1]]), o)
        offs.append(o)
        o += 1
    rec_i = [k for k, fr in enumerate(frames) if fr[1] == T_RECORD][0]

    # 3. a corrupted frame: reported once, every other frame and the text after it survive
    bad = bytearray(vec)
    bad[offs[rec_i] + HDR_LEN + 100] ^= 0x01
    evs = collect([bytes(bad)])
    g = [e for e in evs if e[0] == "frame"]
    bads = [e for e in evs if e[0] == "bad"]
    if len(bads) != 1 or bads[0][1] != "crc" or len(g) != n_frames - 1:
        fail("corrupted record: %d bad, %d frames (expected 1, %d)" % (len(bads), len(g), n_frames - 1))
    if b"OpenScope 2C53T\r\nBuild" not in b"".join(e[1] for e in evs if e[0] == "text"):
        fail("corrupted record: the shell reply after it was lost")

    # 4. a truncated frame (the device abandoned it) followed by the rest
    cut = offs[rec_i] + 700
    trunc = vec[:cut] + vec[offs[rec_i + 1]:]
    g = [e for e in collect([trunc]) if e[0] == "frame"]
    if [f[2] for f in g] != [f[2] for k, f in enumerate(frames) if k != rec_i]:
        fail("truncated record: following frames not recovered (%s)" % [f[2] for f in g])

    # 5. resynchronisation after garbage, with fake markers and a fake header
    rnd = random.Random(7)
    garbage = bytes(rnd.randrange(256) for _ in range(3000))
    fake = bytes([SOF0, SOF1, 3, 0, 1, 0, 0x10, 0x00])
    fake += struct.pack("<H", crc16(fake))                 # a valid header, no valid payload
    noisy = garbage + bytes([SOF0]) + b"S" + fake + garbage[:50] + bytes([SOF0, SOF1]) + vec
    g = [e for e in collect([noisy[o:o + 37] for o in range(0, len(noisy), 37)]) if e[0] == "frame"]
    if [f[2] for f in g] != [f[2] for f in frames]:
        fail("garbage in front: frames %s, expected %s" % ([f[2] for f in g], [f[2] for f in frames]))

    # 6. unknown type skipped by length; torn record flagged; seq wrap is not a gap
    st = Stats()
    for e in collect([vec]):
        st.event(e)
    if 0x7E not in st.count or st.decode_errors:
        fail("unknown type 0x7E not counted / decode errors %d" % st.decode_errors)
    if st.torn != 1 or st.seq_gaps != 0 or st.crc_errors != 0:
        fail("stats: torn=%d seq_gaps=%d crc=%d (expected 1, 0, 0)" % (st.torn, st.seq_gaps, st.crc_errors))
    if st.gen_missed != 1:            # gen 1000 then 1004: one record in between
        fail("stats: gen_missed=%d, expected 1" % st.gen_missed)
    st2 = Stats()
    for e in collect([bytes(bad)]):
        st2.event(e)
    if st2.crc_errors != 1 or st2.seq_gaps != 1:
        fail("stats on the corrupted stream: crc=%d gaps=%d (expected 1, 1)" % (st2.crc_errors, st2.seq_gaps))

    # 7. plot data from a record: volts and the grid
    rec = decode(T_RECORD, frames[rec_i][4])
    tr = record_traces(rec)
    c0 = tr[0]
    if abs(c0["x"][512]) > 1e-12 or abs(c0["xdiv"] - 5e-6) > 1e-12 or c0["unit"] != "V":
        fail("record_traces: trigger/time grid wrong")
    if abs(c0["y"][0] - (rec["ch"][0]["codes"][0] - 100) * 0.0390625) > 1e-9 or c0["ylim"] != (-4.0, 4.0):
        fail("record_traces: volts/window wrong %r" % (c0["ylim"],))
    try:
        import matplotlib  # noqa: F401
        have_mpl = True
    except ImportError:
        have_mpl = False
    if have_mpl:
        import tempfile
        p = Plot("Agg")
        p.update(rec, 0.0)
        p.update(decode(T_RECORD, [f for f in frames if f[1] == T_RECORD][1][4]), 0.2)
        with tempfile.TemporaryDirectory() as d:
            p.fig.savefig(os.path.join(d, "plot.png"))

    print("scope_stream selftest: %d frames, %d bytes of vectors, plot %s: %s"
          % (n_frames, len(vec), "rendered (Agg)" if have_mpl else "skipped (no matplotlib)",
             "FAIL" if fails else "OK"))
    for m in fails:
        print("  FAIL " + m)
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="CDC serial port of the scope")
    ap.add_argument("--stats", type=float, metavar="N", help="run N seconds and print the rates")
    ap.add_argument("--dump", metavar="FILE", help="write every received byte to FILE")
    ap.add_argument("--plot", action="store_true", help="live plot window (matplotlib)")
    ap.add_argument("--replay", metavar="FILE", help="read a --dump file instead of a device")
    ap.add_argument("--png", metavar="FILE", help="with --replay --plot: render to FILE, no window")
    ap.add_argument("--replay-delay", type=float, default=0.2, help="seconds between replayed records")
    ap.add_argument("--what", nargs="*", choices=["scope", "meter", "props"], help="streams to ask for")
    ap.add_argument("--fast", action="store_true", help="ask for spin-paced packets (stream on ... fast)")
    ap.add_argument("--selftest", action="store_true", help="check the demux/decoders against the C vectors")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if a.replay:
        return run_replay(a)
    if not a.port:
        ap.error("--port is needed (or --replay / --selftest)")
    if not a.stats and not a.plot:
        ap.error("choose --stats N and/or --plot")
    return run_device(a)


if __name__ == "__main__":
    sys.exit(main() or 0)
