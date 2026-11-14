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
                  rate in the title (matplotlib, imported only here).
                  Keys (shown in the window): up/down V/div of the active
                  channel, left/right time/div, 1/2 active channel, e its
                  enable, space run/stop, a autoset, p persistence, f fold,
                  c clear. They are sent as `set` / `ui press` on the same
                  connection; the window follows the PROPS frames.
  --persist [S|inf] colour-graded persistence (decay S seconds, or infinite)
  --fold          overlay every trigger crossing of each record (N periods
                  in a record = N waveforms)
  --replay FILE   run --stats (default) or --plot from a dump, no device
  --selftest      demultiplexer and decoders against the C encoder's
                  vectors (firmware/tests/fixtures/stream/), no device

  python3 scripts/scope_stream.py --port /dev/cu.usbmodemXXXX --stats 20
  python3 scripts/scope_stream.py --port /dev/cu.usbmodemXXXX --plot --dump cap.bin
  python3 scripts/scope_stream.py --replay cap.bin --plot
  python3 scripts/scope_stream.py --port socket://127.0.0.1:7321 --plot --persist 2 --fold   # via scope_broker.py

pyserial is needed only to talk to a device; it is imported lazily.
"""
import argparse
import binascii
import math
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
FLAG_REPLAY = 0x01          # header flags: a copy re-sent from a broker's cache
FLAG_FILTERED = 0x02        # a broker left other types out of this client's stream: seq gaps expected
TRIG_NONE = 0xFFFF


def crc16(data):
    """CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF)."""
    return binascii.crc_hqx(bytes(data), 0xFFFF)


def crc32(data):
    return zlib.crc32(bytes(data)) & 0xFFFFFFFF


def encode_frame(ftype, seq, payload, flags=0):
    """One frame on the wire (stream_frame.c's layout, pad byte included)."""
    payload = bytes(payload)
    h = struct.pack("<BBBBHH", SOF0, SOF1, ftype, flags, seq & 0xFFFF, len(payload))
    h += struct.pack("<H", crc16(h))
    out = h + payload + struct.pack("<I", crc32(h[2:] + payload))
    if len(out) % 64 == 0:
        out += b"\x00"
    return out


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


# ── payload encoders (the decoders' inverse: fake devices, synthetic tests) ──
def _pstr(v):
    b = v.encode("latin-1") if isinstance(v, str) else bytes(v)
    return bytes([len(b)]) + b


def encode_payload(d):
    """Payload bytes for a decoded dict (the types this host decodes)."""
    t = d["type"]
    if t == T_HELLO:
        return struct.pack("<BBBBHHHI", d["proto_major"], d["proto_minor"], d["caps"], d["on"], d["max_payload"],
                           d["rec_samples"], d["n_props"], d["table_crc"]) + _pstr(d["fw"])
    if t == T_PROPDESC:
        return struct.pack("<HBBBBHiii", d["id"], d["type_"], d["unit"], d["decimals"], d["group"], d["flags"],
                           d["min"], d["max"], d["def"]) + _pstr(d["name"]) + _pstr(d["unit_sym"])
    if t == T_PROPS:
        out = struct.pack("<IB", d["table_crc"], len(d["epochs"])) + b"".join(struct.pack("<I", e) for e in d["epochs"])
        return out + struct.pack("<H", len(d["props"])) + b"".join(struct.pack("<Hi", i, v) for i, v in d["props"])
    if t == T_RECORD:
        out = struct.pack("<IIffHHBBBB", d["gen"], d["t_ms"], d["fs_hz"], d["s_per_div"], d["n"], d["trig_idx"],
                          d["flags"], d["kind"], d["tb_code"], len(d["ch"]))
        for c in d["ch"]:
            out += struct.pack("<BBBBhHff", c["id"], c["vdiv_idx"], c["zero_code"], c["coupling"], c["position"],
                               c["probe_x"], c["volts_per_count"], c["volts_per_div"])
        return out + b"".join(bytes(c["codes"]) for c in d["ch"]) + struct.pack("<I", d.get("gen_end", d["gen"]))
    if t == T_METER:
        return struct.pack("<IIfiBBBBBBB", d["update_count"], d["t_ms"], d["value"], d["bcd"], d["func"],
                           d["auto_fn"], d["result_class"], d["status_code"], d["decimal_pos"], d["range_index"],
                           d["flags"]) + _pstr(d["unit"]) + _pstr(d["display"]) + _pstr(d["range_label"])
    if t == T_STATS:
        out = struct.pack("<IIIII", d["t_ms"], d["bytes"], d["rec_missed"], d["rec_torn"], d["aborts"])
        ts = sorted(d["sent"])
        return out + bytes([len(ts)]) + b"".join(struct.pack("<BII", k, d["sent"][k], d["dropped"][k]) for k in ts)
    raise ValueError("no encoder for type %d" % t)


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
        self.replayed = 0
        self.filtered = 0
        self.rec_kinds = {}       # (kind, has trigger index) -> records

    def props_by_name(self):
        """Latest PROPS as {name: value} (names from PROPDESC)."""
        return {self.names[i]["name"]: v for i, v in self.props.items() if i in self.names}

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
        _, ftype, seq, flags, payload = ev
        if flags & FLAG_REPLAY:
            # a broker's cached copy for a late joiner: not part of the live
            # sequence, so neither a gap nor a rate
            self.replayed += 1
            self.last_seq = None
        else:
            self.frame_bytes += HDR_LEN + len(payload) + CRC_LEN
            if flags & FLAG_FILTERED:
                self.filtered += 1
            elif self.last_seq is not None and ftype != T_HELLO:
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
            k = (d["kind"], d["trig_idx"] != TRIG_NONE)
            self.rec_kinds[k] = self.rec_kinds.get(k, 0) + 1
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
        p("seq_gaps=%s crc_errors=%d truncated=%d decode_errors=%d replayed=%d" % (
            "n/a (broker-filtered stream; see !broker status)" if self.filtered else self.seq_gaps,
            self.crc_errors, self.truncated, self.decode_errors, self.replayed))
        p("records=%d torn=%d gen_missed=%d (records published but not received)" % (
            self.records, self.torn, self.gen_missed))
        if self.rec_kinds:
            p("records by kind: " + " ".join("%s/%s=%d" % (KIND_NAMES.get(k, k).replace(" ", "_"),
                                                           "index" if i else "no_index", n)
                                             for (k, i), n in sorted(self.rec_kinds.items())))
        if self.fw_stats:
            s = self.fw_stats
            p("firmware: bytes=%d rec_missed=%d rec_torn=%d aborts=%d" % (
                s["bytes"], s["rec_missed"], s["rec_torn"], s["aborts"]))
            p("firmware sent/dropped: " + " ".join(
                "%s=%d/%d" % (TYPE_NAMES.get(t, t), s["sent"][t], s["dropped"][t])
                for t in sorted(s["sent"]) if s["sent"][t] or s["dropped"][t]))


# ── trigger, folding and persistence: the numeric core (pure) ───────────────
SCREEN_DIVS_X, SCREEN_DIVS_Y = 12, 8
HEAD_SKIP = 128          # the record's unreliable head (firmware scope_record.h)
TRIG_PX_SPAN = 206       # trig.level is in screen pixels; 206 px = 256 codes (scope_ui.c)
KIND_NAMES = {0: "held", 1: "Trig'd", 2: "Auto (free-run)", 3: "Auto (free-run, split)"}


def robust_span(codes, lo=HEAD_SKIP, trim=0.02):
    """(low, high) of codes[lo:] with the extreme `trim` fraction cut on each
    side, so one-sample outliers at the edges do not move the midline."""
    s = sorted(codes[lo:]) if len(codes) > lo else sorted(codes)
    if not s:
        return 0, 0
    k = int(len(s) * trim)
    return s[k], s[len(s) - 1 - k]


def hw_level_code(level_px):
    """Record code of the hardware trigger level for trig.level (fpga.c:
    reg 0x08 = 128 + level * 256 / 206 in C integer division, records are
    raw - 28)."""
    lv = max(-100, min(100, int(level_px)))
    q = abs(lv) * 256 // TRIG_PX_SPAN
    code = 128 + (q if lv >= 0 else -q)
    return min(254, max(1, code)) - 28


def soft_level(codes, level_px=0, lo=HEAD_SKIP):
    """The scope's soft-trigger threshold (scope_ui.c): the signal's midline
    plus trig.level converted to codes, kept inside the signal. None when the
    record is flat (span < 8 codes), as on the screen."""
    mn, mx = robust_span(codes, lo)
    if mx - mn < 8:
        return None
    thr = (mn + mx) / 2.0 + level_px * 256.0 / TRIG_PX_SPAN
    return min(max(thr, mn + 1), mx - 1)


def find_crossings(codes, level, rising=True, hyst=3, confirm=2, lo=HEAD_SKIP):
    """Every crossing of `level` in the chosen direction, as a fractional
    sample index (linear interpolation between the two samples around it).

    Schmitt-armed: after a crossing, the signal must go back past level -/+
    hyst before the next one counts. A crossing whose next `confirm` samples
    fall back is a spike and does not count (the scope's three-point
    confirm), so one-sample outliers at a square's edges give one crossing
    per edge."""
    n = len(codes)
    out = []
    if level is None or n < 3:
        return out
    lo = max(lo, 1)
    arm_at = level - hyst if rising else level + hyst
    p = codes[lo - 1]
    armed = p <= arm_at if rising else p >= arm_at
    for i in range(lo, n - confirm):
        a, b = codes[i - 1], codes[i]
        if armed and ((a < level <= b) if rising else (a > level >= b)):
            ok = all((codes[i + k] >= level) if rising else (codes[i + k] <= level)
                     for k in range(1, confirm + 1))
            if ok:
                out.append(i - 1 + (level - a) / float(b - a))
                armed = False
        if (b <= arm_at) if rising else (b >= arm_at):
            armed = True
    return out


def median_period(crossings):
    if len(crossings) < 2:
        return None
    d = sorted(b - a for a, b in zip(crossings, crossings[1:]))
    return d[len(d) // 2]


def fold_pieces(codes, crossings, half, lo=HEAD_SKIP):
    """Every piece [c - half, c + half] of the record around each crossing c,
    aligned at the crossing: a list of (offsets in samples, codes), one per
    crossing. A record with N crossings contributes N waveforms."""
    out = []
    n = len(codes)
    for c in crossings:
        a = max(lo, int(c - half) + 1)
        b = min(n - 1, int(c + half))
        out.append(([i - c for i in range(a, b + 1)], [codes[i] for i in range(a, b + 1)]))
    return out


def fold_points(codes, crossings, half, lo=HEAD_SKIP):
    """fold_pieces flattened: (offsets, codes) of all pieces together."""
    xs, ys = [], []
    for px, py in fold_pieces(codes, crossings, half, lo):
        xs += px
        ys += py
    return xs, ys


class Hist:
    """2-D histogram (x across, y up) with exponential decay or infinite
    accumulation, in a flat array of doubles (numpy reads it without a copy).

    Decay without touching every cell: a sample added at time t weighs
    exp((t - t0) / tau); the value shown at time `now` is the stored count
    times exp(-(now - t0) / tau). When the weights grow large, everything is
    renormalised once (t0 moves to now)."""

    def __init__(self, nx, ny, x0, x1, y0, y1, tau=None):
        from array import array
        self.nx, self.ny = nx, ny
        self.x0, self.x1, self.y0, self.y1 = x0, x1, y0, y1
        self.tau = tau
        self.t0 = None
        self.cells = array("d", bytes(8 * nx * ny))
        self.waveforms = 0

    def clear(self):
        for i in range(len(self.cells)):
            self.cells[i] = 0.0
        self.t0 = None
        self.waveforms = 0

    def _weight(self, t):
        if self.tau is None:
            return 1.0
        if self.t0 is None:
            self.t0 = t
        e = (t - self.t0) / self.tau
        if e > 30.0:                          # renormalise: weights ~1e13
            k = math.exp(-e)
            for i in range(len(self.cells)):
                self.cells[i] *= k
            self.t0, e = t, 0.0
        return math.exp(e)

    def add(self, xs, ys, t=0.0, waveforms=1):
        w = self._weight(t)
        sx = self.nx / float(self.x1 - self.x0)
        sy = self.ny / float(self.y1 - self.y0)
        c, nx, ny = self.cells, self.nx, self.ny
        for x, y in zip(xs, ys):
            ix = int((x - self.x0) * sx)
            iy = int((y - self.y0) * sy)
            if 0 <= ix < nx and 0 <= iy < ny:
                c[iy * nx + ix] += w
        self.waveforms += waveforms

    def add_pieces(self, pieces, t=0.0, waveforms=1):
        """Each piece is (xs, ys): draw it as a line through its samples, as
        a scope's vector display does (a steep edge fills the cells it
        crosses), one count per cell per segment."""
        w = self._weight(t)
        sx = self.nx / float(self.x1 - self.x0)
        sy = self.ny / float(self.y1 - self.y0)
        c, nx, ny = self.cells, self.nx, self.ny
        for xs, ys in pieces:
            px = py = None
            for x, y in zip(xs, ys):
                fx, fy = (x - self.x0) * sx, (y - self.y0) * sy
                if px is None:
                    steps = 0
                else:
                    steps = int(max(abs(fx - px), abs(fy - py)))
                if steps <= 1:
                    ix, iy = int(fx), int(fy)
                    if 0 <= ix < nx and 0 <= iy < ny:
                        c[iy * nx + ix] += w
                else:
                    for k in range(1, steps + 1):
                        u = k / float(steps)
                        ix, iy = int(px + (fx - px) * u), int(py + (fy - py) * u)
                        if 0 <= ix < nx and 0 <= iy < ny:
                            c[iy * nx + ix] += w
                px, py = fx, fy
        self.waveforms += waveforms

    def scale(self, now):
        """Multiply stored cells by this to get counts as of `now`."""
        if self.tau is None or self.t0 is None:
            return 1.0
        return math.exp(-(now - self.t0) / self.tau)

    def value(self, ix, iy, now=0.0):
        return self.cells[iy * self.nx + ix] * self.scale(now)


def geometry_key(rec, props=None):
    """What must stay the same for accumulated pictures to stay valid: from
    the record (rate, channels, scale, zero) and, when known, the properties
    (V/div, position, timebase, trigger, probe, coupling, enables)."""
    k = [rec["fs_hz"], rec["nch"]]
    for c in rec["ch"]:
        k += [c["id"], c["vdiv_idx"], c["zero_code"], c["probe_x"], c["coupling"], c["volts_per_count"]]
    if props:
        for nm in GEOMETRY_PROPS:
            k.append(props.get(nm))
    return tuple(k)


GEOMETRY_PROPS = ("ch1.enabled", "ch1.vdiv", "ch1.pos", "ch1.probe", "ch1.coupling",
                  "ch2.enabled", "ch2.vdiv", "ch2.pos", "ch2.probe", "ch2.coupling",
                  "tb.code", "tb.roll", "tb.rollstep", "trig.mode", "trig.edge", "trig.source",
                  "trig.level", "trig.x", "il.enabled")


def trig_settings(props):
    """(level_px, rising, source channel 0/1) from PROPS by name."""
    p = props or {}
    return p.get("trig.level", 0), p.get("trig.edge", 0) == 0, p.get("trig.source", 0)


def record_reference(rec, props=None):
    """Sample index to put at t = 0, and how it was found:
    "hw" the hardware trigger of a time-ordered record; "soft" the scope's
    soft-trigger crossing nearest the record centre (a triggered record that
    is not time-ordered, e.g. a fast periodic signal: the screen aligns it the
    same way); "centre" nothing to lock to."""
    n = rec["n"]
    level_px, rising, src = trig_settings(props)
    chans = rec["ch"]
    if rec["trig_idx"] != TRIG_NONE:
        # the screen's hardware anchor (trig_edge_anchor): the crossing of the
        # level in force nearest the index, within +-48 samples, on CH1 (the
        # level was measured there); else the index itself
        t = rec["trig_idx"]
        c1 = next((c for c in chans if c["id"] == 0), None)
        if c1 is not None and src == 0:
            xs = [x for x in find_crossings(c1["codes"], hw_level_code(level_px), rising, lo=max(1, t - 64))
                  if abs(x - t) <= 48]
            if xs:
                return min(xs, key=lambda v: abs(v - t)), "hw"
        return t, "hw"
    c = next((c for c in chans if c["id"] == src), chans[0] if chans else None)
    if c is not None:
        xs = find_crossings(c["codes"], soft_level(c["codes"], level_px), rising)
        if xs:
            return min(xs, key=lambda v: abs(v - n / 2.0)), "soft"
    return n // 2, "centre"


def channel_scale(c):
    """(volts per code, zero code, unit, step per division, y window)."""
    vpc, vpd, z = c["volts_per_count"], c["volts_per_div"], c["zero_code"]
    if vpc > 0 and vpd > 0:
        centre = (100 - z) * vpc              # screen centre code 100 (true scale)
        return vpc, z, "V", vpd, (centre - SCREEN_DIVS_Y / 2 * vpd, centre + SCREEN_DIVS_Y / 2 * vpd)
    return 1.0, 0, "code", 25.0, (128.0 - 100.0, 128.0 + 100.0)


def record_traces(rec, props=None, ref=None):
    """RECORD dict -> per-channel dicts with x (s from the reference), y (V or
    codes), unit, volts/div, the y window (8 divisions around the screen
    centre) and the x grid step (time/div)."""
    n, fs = rec["n"], rec["fs_hz"]
    if ref is None:
        ref, _ = record_reference(rec, props)
    dt = 1.0 / fs if fs > 0 else 1.0
    x = [(i - ref) * dt for i in range(n)]
    spd = rec["s_per_div"] if rec["s_per_div"] > 0 else 25 * dt
    out = []
    for c in rec["ch"]:
        k, z, unit, step, ylim = channel_scale(c)
        y = [(v - z) * k for v in c["codes"]]
        out.append({"name": "CH%d" % (c["id"] + 1), "x": x, "y": y, "unit": unit, "ydiv": step,
                    "ylim": ylim, "xdiv": spd})
    return out


def fold_record(rec, props=None, half=None):
    """Fold every enabled channel at the trigger source's crossings.
    Returns (crossings, half window in samples, {channel id: [(xs s, ys V), ...]}),
    one piece per crossing."""
    level_px, rising, src = trig_settings(props)
    chans = rec["ch"]
    s = next((c for c in chans if c["id"] == src), chans[0] if chans else None)
    if s is None:
        return [], 0, {}
    xs = find_crossings(s["codes"], soft_level(s["codes"], level_px), rising)
    if half is None:
        p = median_period(xs)
        half = min(150.0, p) if p else 150.0     # two periods, or the screen's 300 samples
    dt = 1.0 / rec["fs_hz"] if rec["fs_hz"] > 0 else 1.0
    out = {}
    for c in chans:
        k, z, _u, _s, _yl = channel_scale(c)
        out[c["id"]] = [([v * dt for v in px], [(v - z) * k for v in py])
                        for px, py in fold_pieces(c["codes"], xs, half)]
    return xs, half, out


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


# ── keyboard controls: key -> shell command (pure) ──────────────────────────
KEYMAP = (
    ("up", "V/div up (active ch)"),
    ("down", "V/div down"),
    ("right", "time/div slower"),
    ("left", "time/div faster"),
    ("1 / 2", "active channel"),
    ("e", "active ch on/off"),
    (" ", "run / stop"),
    ("a", "autoset"),
    ("p", "persistence on/off"),
    ("f", "fold at trigger"),
    ("c", "clear persistence"),
)


def key_command(key, active, props):
    """Shell command for a key, or None (local keys are handled by the plot).
    `active` is 0/1; props the latest PROPS by name."""
    p = props or {}
    ch = "ch%d" % (active + 1)
    if key == "up":
        return "set %s.vdiv +=1" % ch
    if key == "down":
        return "set %s.vdiv -=1" % ch
    if key == "right":
        return "set tb.code +=1"
    if key == "left":
        return "set tb.code -=1"
    if key == " ":
        return "set view.run %s" % ("off" if p.get("view.run", 1) else "on")
    if key == "e":
        return "set %s.enabled %s" % (ch, "off" if p.get(ch + ".enabled", 1) else "on")
    if key == "a":
        return "ui press AUTO"
    return None


# ── live window ─────────────────────────────────────────────────────────────
COLORS = {"CH1": "#2a78d6", "CH2": "#eb6834"}   # categorical slots 1, 2
SURFACE, INK, INK2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"
# Colour grading (rare -> frequent), the oscilloscope convention the owner
# asked for: blue, cyan, yellow, red.
GRADE = ("#1f3fbf", "#19a7c9", "#f2d22e", "#d8312b")
HIST_NX, HIST_NY = 400, 200


class Plot:
    """Live window: one panel per channel (each with its own V/div grid; no
    shared second y-axis), shared time axis with the time/div grid, optional
    colour-graded persistence under the latest trace, optional folding."""

    def __init__(self, backend=None, send=None, persist=None, fold=False):
        import matplotlib
        if backend:
            matplotlib.use(backend)
        import matplotlib.pyplot as plt
        from matplotlib.colors import LinearSegmentedColormap, LogNorm
        from matplotlib.ticker import MultipleLocator
        import numpy as np
        self.np, self.plt, self.ML, self.LogNorm = np, plt, MultipleLocator, LogNorm
        self.cmap = LinearSegmentedColormap.from_list("grade", GRADE)
        self.cmap.set_bad(alpha=0.0)
        self.send = send
        self.persist = persist            # None = off, float = decay seconds, "inf"
        self.persist_on = persist is not None
        self.fold = fold
        self.active = 0
        self.props = {}
        self.fig = None
        self.layout = None
        self.geom = None
        self.hists = {}
        self.times = []
        self.wf_times = []
        self.last_reply = ""
        self.last_rec = None

    # -- layout --
    def _build(self, names):
        plt = self.plt
        if self.fig is None:
            plt.ion()
            self.fig = plt.figure(figsize=(11, 7), facecolor=SURFACE)
            self.fig.canvas.mpl_connect("key_press_event", self._on_key)
            for k in [k for k in plt.rcParams if k.startswith("keymap.")]:
                plt.rcParams[k] = []            # our keys, not matplotlib's
        self.fig.clf()
        self.axes, self.lines, self.images = {}, {}, {}
        first = None
        for k, nm in enumerate(names):
            ax = self.fig.add_axes([0.08, 0.16 + (len(names) - 1 - k) * 0.74 / len(names),
                                    0.68, 0.74 / len(names) - 0.04], sharex=first)
            first = first or ax
            ax.set_facecolor(SURFACE)
            ax.grid(True, which="both", color=GRID, linewidth=0.8, zorder=0)
            ax.tick_params(colors=INK2, labelsize=8)
            for s in ax.spines.values():
                s.set_color(GRID)
            self.images[nm] = None
            (ln,) = ax.plot([], [], color=COLORS.get(nm, INK), linewidth=1.5, zorder=3)
            self.axes[nm], self.lines[nm] = ax, ln
        self.keytext = self.fig.text(0.79, 0.90, "", va="top", ha="left", fontsize=8, color=INK2,
                                     family="monospace")
        self.status = self.fig.text(0.08, 0.04, "", va="bottom", ha="left", fontsize=9, color=INK)
        self.layout = tuple(names)
        self.hists = {}

    # -- keys --
    def _on_key(self, ev):
        key = ev.key
        if key in ("1", "2"):
            self.active = int(key) - 1
        elif key == "p":
            self.persist_on = not self.persist_on
            self.hists = {}
        elif key == "f":
            self.fold = not self.fold
            self.hists = {}
        elif key == "c":
            self.hists = {}
        else:
            cmd = key_command(key, self.active, self.props)
            if cmd and self.send:
                self.send(cmd)
                self.last_reply = "> " + cmd
        self.redraw()

    def reply(self, text):
        line = [l for l in text.replace("\r", "").split("\n") if l.strip() and l.strip() != ">"]
        if line:
            self.last_reply = line[-1][-90:]
            self.redraw()

    def set_props(self, props):
        """Follow the scope's state: also when no record comes (stopped), so
        a change made with the scope's own keys shows at once."""
        self.props = dict(props)
        self.redraw()

    def redraw(self):
        if self.last_rec is not None:
            self.update(self.last_rec, time.monotonic(), redraw_only=True)

    # -- drawing --
    def _hist_for(self, nm, xlim, ylim):
        h = self.hists.get(nm)
        if h is None:
            tau = None if self.persist in (None, "inf") else float(self.persist)
            h = self.hists[nm] = Hist(HIST_NX, HIST_NY, xlim[0], xlim[1], ylim[0], ylim[1], tau)
        return h

    def update(self, rec, now, redraw_only=False):
        np = self.np
        tr = record_traces(rec, self.props)
        names = [t["name"] for t in tr]
        if self.layout != tuple(names):
            self._build(names)
        g = geometry_key(rec, self.props)
        if g != self.geom:
            self.geom, self.hists = g, {}
        if not redraw_only:
            self.last_rec = rec
            self.times = [t for t in self.times if now - t < 2.0] + [now]
        fps = (len(self.times) - 1) / (self.times[-1] - self.times[0]) if len(self.times) > 1 else 0.0
        k, tu = time_scale(tr[0]["xdiv"])
        folded, ncross = None, 1
        if self.fold:
            xs, half, folded = fold_record(rec, self.props)
            ncross = len(xs)
            if not redraw_only:
                self.wf_times = [t for t in self.wf_times if now - t[0] < 2.0] + [(now, len(xs))]
            dt = 1.0 / rec["fs_hz"] if rec["fs_hz"] > 0 else 1.0
            xlim = (-half * dt, half * dt)
        else:
            xlim = (tr[0]["x"][0], tr[0]["x"][-1])
        for t, c in zip(tr, rec["ch"]):
            nm = t["name"]
            ax, ln = self.axes[nm], self.lines[nm]
            if folded is not None:
                pieces = folded.get(c["id"], [])
                ln.set_data([], [])
                ln.set_visible(False)
            else:
                pieces = [(t["x"], t["y"])]
                ln.set_visible(True)
                ln.set_data([v * k for v in t["x"]], t["y"])
            ax.set_ylim(*t["ylim"])
            ax.yaxis.set_major_locator(self.ML(t["ydiv"]))
            span_divs = (xlim[1] - xlim[0]) / t["xdiv"] if t["xdiv"] > 0 else 0
            # one grid line per division; labels every 5 when the view is much
            # wider than the screen's 12 divisions
            ax.xaxis.set_minor_locator(self.ML(t["xdiv"] * k))
            ax.xaxis.set_major_locator(self.ML(t["xdiv"] * k * (5 if span_divs > 2 * SCREEN_DIVS_X else 1)))
            ax.set_xlim(xlim[0] * k, xlim[1] * k)
            ax.tick_params(axis="x", labelbottom=(t is tr[-1]))
            ax.set_ylabel("%s  %s/div%s" % (nm, eng(t["ydiv"], t["unit"]) if t["unit"] == "V"
                                            else "%g code" % t["ydiv"],
                                            "  [active]" if c["id"] == self.active else ""),
                          color=INK, fontsize=9)
            show_hist = self.persist_on or folded is not None
            img = self.images.get(nm)
            if show_hist:
                h = self._hist_for(nm, xlim, t["ylim"])
                if not redraw_only:
                    h.add_pieces(pieces, now, waveforms=ncross)
                a = np.frombuffer(h.cells, dtype=np.float64).reshape(h.ny, h.nx) * h.scale(now)
                m = np.ma.masked_less_equal(a, 0.0)
                vmax = float(m.max()) if m.count() else 1.0
                norm = self.LogNorm(vmin=max(vmax * 1e-3, 1e-12), vmax=max(vmax, 1e-12))
                ext = (h.x0 * k, h.x1 * k, h.y0, h.y1)
                if img is None:
                    img = ax.imshow(m, origin="lower", extent=ext, aspect="auto", cmap=self.cmap,
                                    norm=norm, interpolation="nearest", zorder=2)
                    self.images[nm] = img
                else:
                    img.set_data(m)
                    img.set_extent(ext)
                    img.set_norm(norm)
                img.set_visible(True)
            elif img is not None:
                img.set_visible(False)
        last = self.axes[names[-1]]
        last.set_xlabel("time from trigger (%s), grid %s/div" % (tu, eng(tr[0]["xdiv"], "s")), color=INK2,
                        fontsize=9)
        _ref, how = record_reference(rec, self.props)
        trig = KIND_NAMES.get(rec["kind"], "kind %d" % rec["kind"])
        if rec["kind"] == 1:
            trig += {"hw": "", "soft": " (soft-aligned)", "centre": " (no crossing)"}[how]
        flags = []
        if rec["torn"]:
            flags.append("TORN")
        if rec["flags"] & REC_INTERLEAVED:
            flags.append("interleaved")
        if (self.props.get("view.run", 1) if "view.run" in self.props else rec["flags"] & REC_RUNNING) == 0:
            flags.append("STOP")
        self.fig.suptitle("OpenScope stream  %.1f records/s  %s  fs %s  gen %d %s"
                          % (fps, trig, eng(rec["fs_hz"], "Sa/s"), rec["gen"], " ".join(flags)),
                          color=INK, fontsize=11)
        mode = []
        if self.fold:
            n = sum(c for _t, c in self.wf_times)
            span = (self.wf_times[-1][0] - self.wf_times[0][0]) if len(self.wf_times) > 1 else 0
            wf = max([h.waveforms for h in self.hists.values()] or [0])
            mode.append("fold: %d waveforms accumulated, %s/s (%d in this record)" % (
                wf, "%.0f" % (n / span) if span > 0 else "--", self.wf_times[-1][1] if self.wf_times else 0))
        if self.persist_on or self.fold:
            mode.append("persistence %s" % ("infinite" if self.persist in (None, "inf")
                                            else "%gs decay" % float(self.persist)))
        self.status.set_text("   ".join(mode + ([self.last_reply] if self.last_reply else [])))
        self.keytext.set_text("keys\n" + "\n".join("%-6s %s" % ("space" if a == " " else a, b)
                                                    for a, b in KEYMAP))
        self.fig.canvas.draw_idle()
        self.fig.canvas.flush_events()

    def alive(self):
        return self.fig is None or self.plt.fignum_exists(self.fig.number)

    def pause(self):
        self.plt.pause(0.001)


# ── device / replay loops ───────────────────────────────────────────────────
def open_port(port):
    """A serial device, or any pyserial URL: socket://127.0.0.1:PORT reaches
    scripts/scope_broker.py, which shares one scope between several tools."""
    try:
        import serial
    except ImportError:
        sys.exit("pyserial is needed to talk to the device (pip install pyserial)")
    s = serial.serial_for_url(port, baudrate=115200, timeout=0.02)
    try:
        s.reset_input_buffer()
    except Exception:
        pass
    return s


def _persist_arg(v):
    if v is None:
        return None
    return "inf" if v == "inf" else float(v)


def run_device(a):
    ser = open_port(a.port)
    dump = open(a.dump, "wb") if a.dump else None
    dm, st = Demuxer(), Stats()

    def send(cmd):
        ser.write(cmd.encode() + b"\r")

    plot = Plot(send=send, persist=_persist_arg(a.persist), fold=a.fold) if a.plot else None
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
                    if plot:
                        if ev[0] == "text":
                            plot.reply(ev[1].decode("utf-8", "replace"))
                        elif d and d["type"] == T_PROPS:
                            plot.set_props(st.props_by_name())
                        elif d and d["type"] == T_RECORD:
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
    plot = Plot("Agg" if a.png else None, persist=_persist_arg(a.persist), fold=a.fold) if a.plot else None
    step = 4096
    t_fake = 0.0
    for o in range(0, len(data), step):
        chunk = data[o:o + step]
        st.add_bytes(len(chunk), 0.0)       # no wall clock in a dump: rates come from device t_ms
        for ev in dm.feed(chunk, final=(o + step >= len(data))):
            d = st.event(ev, None)
            if plot and d and d["type"] == T_PROPS:
                plot.set_props(st.props_by_name())
            if plot and d and d["type"] == T_RECORD:
                t_fake += a.replay_delay if not a.png else 0.2
                plot.update(d, t_fake)
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


def synth_record(ch1, ch2=None, fs=5e6, trig_idx=TRIG_NONE, kind=1, vdiv=3, zero=100, vpc=0.04):
    chans = []
    for i, codes in enumerate([ch1, ch2]):
        if codes is None:
            continue
        chans.append({"id": i, "vdiv_idx": vdiv, "zero_code": zero, "coupling": 0, "position": 0, "probe_x": 1,
                      "volts_per_count": vpc, "volts_per_div": vpc * 25, "codes": bytes(codes)})
    return {"type": T_RECORD, "gen": 2, "gen_end": 2, "torn": False, "t_ms": 0, "fs_hz": fs, "s_per_div": 5e-6,
            "n": len(ch1), "trig_idx": trig_idx, "flags": REC_RUNNING, "kind": kind, "tb_code": 10,
            "nch": len(chans), "ch": chans}


def square(n=1024, period=50, lo=60, hi=160, phase=0, outliers=False):
    """A square; with outliers, a one-sample spike at every edge, alternating
    overshoot (255 just after a rising edge), a dip (0 just before one),
    and a sample stuck on the wrong side just after a falling edge."""
    out = []
    for i in range(n):
        out.append(hi if ((i + phase) % period) < period // 2 else lo)
    edges = [i for i in range(1, n) if out[i - 1] == lo and out[i] == hi]
    if outliers:
        for k, e in enumerate(edges):
            if k % 3 == 0 and e + 1 < n:
                out[e + 1] = 255
            elif k % 3 == 1 and e - 1 >= 0:
                out[e - 1] = 0
            elif k % 3 == 2 and e - 2 >= 0:
                out[e - 2] = 255                 # a spike above the level before the edge
    return out, edges


def numeric_selftest(fail):
    # square, clean: one crossing per rising edge past the head, at the edge
    codes, edges = square()
    want = [e for e in edges if e >= HEAD_SKIP + 1]
    lvl = soft_level(codes)
    if lvl is None or abs(lvl - 110) > 1:
        fail("soft_level of a 60/160 square = %r, expected 110" % lvl)
    xs = find_crossings(codes, lvl, True)
    if len(xs) != len(want) or any(abs(x - (e - 0.5)) > 0.51 for x, e in zip(xs, want)):
        fail("clean square: crossings %s, expected near %s" % ([round(x, 2) for x in xs[:4]], want[:4]))
    if abs(median_period(xs) - 50) > 1e-9:
        fail("median period %r, expected 50" % median_period(xs))
    fx = find_crossings(codes, lvl, False)
    if len(fx) != len([i for i in range(HEAD_SKIP + 1, 1024) if codes[i - 1] == 160 and codes[i] == 60]):
        fail("falling crossings %d" % len(fx))

    # square with one-sample outliers at the edges: still one crossing per
    # edge, within 2 samples, and the midline does not move
    codes, edges = square(outliers=True)
    lvl2 = soft_level(codes)
    if lvl2 is None or abs(lvl2 - 110) > 1:
        fail("soft_level with outliers = %r, expected 110" % lvl2)
    xs = find_crossings(codes, lvl2, True)
    want = [e for e in edges if e >= HEAD_SKIP + 2]
    got = [x for x in xs if x >= HEAD_SKIP + 1]
    if len(got) != len(want) or any(abs(x - e) > 2.0 for x, e in zip(got, want)):
        fail("square with outliers: %d crossings %s, expected %d near %s"
             % (len(got), [round(x, 1) for x in got[:6]], len(want), want[:6]))

    # a record with a single edge: one crossing, one waveform, the default
    # window (the screen's 300 samples)
    step = [60] * 600 + [160] * 424
    rec = synth_record(step)
    xs, half, out = fold_record(rec)
    if len(xs) != 1 or abs(xs[0] - 599.5) > 0.01 or half != 150.0:
        fail("single edge: crossings %s half %s" % (xs, half))
    fxs, fys = [v for pc in out[0] for v in pc[0]], [v for pc in out[0] for v in pc[1]]
    if len(out[0]) != 1 or len(fxs) != 300 or any((y > 0) != (x > 0) for x, y in zip(fxs, fys)):
        fail("single edge fold: %d points, sides wrong" % len(fxs))
    ref, how = record_reference(rec)
    if how != "soft" or abs(ref - 599.5) > 0.01:
        fail("record_reference: %s %s (a triggered record without an index aligns soft)" % (ref, how))
    if record_reference(synth_record(step, trig_idx=512))[1] != "hw":
        fail("record_reference ignores the hardware index")
    if hw_level_code(0) != 100 or hw_level_code(-50) != 128 - 62 - 28 or hw_level_code(100) != 252 - 28:
        fail("hw_level_code %s" % [hw_level_code(0), hw_level_code(-50), hw_level_code(100)])
    # time-ordered records whose edge wanders a few samples around 512 (the
    # measured 501..520) accumulate to one edge: anchored at the crossing
    for shift in (-9, -3, 0, 4, 8):
        rec = synth_record([50] * (512 + shift) + [150] * (512 - shift), trig_idx=512)
        ref, how = record_reference(rec)
        if how != "hw" or abs(ref - (511.5 + shift)) > 0.01:
            fail("hw anchor at shift %d: %s %s" % (shift, ref, how))
    far = synth_record([50] * 600 + [150] * 424, trig_idx=512)
    if record_reference(far)[0] != 512:
        fail("hw anchor: a crossing 88 samples away must not be taken")
    if record_reference(synth_record([100] * 1024))[1] != "centre":
        fail("record_reference on a flat record")

    # folding a 20-period square: N waveforms, aligned at the crossing
    codes, edges = square(phase=7)
    rec = synth_record(codes, codes)
    xs, half, out = fold_record(rec)
    if len(xs) < 17 or half != 50:
        fail("fold: %d crossings, half %s" % (len(xs), half))
    dt = 1 / 5e6
    for cid in (0, 1):
        if len(out[cid]) != len(xs):
            fail("fold ch%d: %d pieces for %d crossings" % (cid, len(out[cid]), len(xs)))
        fxs, fys = [v for pc in out[cid] for v in pc[0]], [v for pc in out[cid] for v in pc[1]]
        after = [y for x, y in zip(fxs, fys) if 0.5 * dt < x < 20 * dt]
        before = [y for x, y in zip(fxs, fys) if -20 * dt < x < -0.5 * dt]
        if not after or not before or min(after) <= 0 or max(before) >= 0:
            fail("fold ch%d: not aligned (after %s, before %s)" % (cid, after[:3], before[:3]))
    # trigger level shifts the threshold; edge selects the direction
    p = {"trig.level": 50, "trig.edge": 1, "trig.source": 1}
    if abs(soft_level(codes, 20) - (110 + 20 * 256 / 206)) > 1e-9 or soft_level(codes, 100) != 159:
        fail("soft_level with trig.level")
    if trig_settings(p) != (50, False, 1):
        fail("trig_settings %r" % (trig_settings(p),))

    # histogram: binning, infinite accumulation, decay, renormalisation
    h = Hist(10, 10, 0.0, 1.0, 0.0, 1.0)
    h.add([0.05, 0.05, 0.95, 1.5, -0.1], [0.05, 0.05, 0.95, 0.5, 0.5])
    if h.value(0, 0) != 2 or h.value(9, 9) != 1 or sum(h.cells) != 3:
        fail("Hist binning: %s" % [h.value(0, 0), h.value(9, 9), sum(h.cells)])
    h.add([0.05], [0.05], t=100.0)
    if h.value(0, 0, now=1e6) != 3:
        fail("Hist infinite accumulation decays")
    h = Hist(4, 4, 0, 1, 0, 1, tau=2.0)
    h.add([0.1], [0.1], t=10.0)
    if abs(h.value(0, 0, now=12.0) - math.exp(-1)) > 1e-12:
        fail("Hist decay after one tau: %r" % h.value(0, 0, now=12.0))
    h.add([0.1], [0.1], t=12.0)
    if abs(h.value(0, 0, now=12.0) - (1 + math.exp(-1))) > 1e-12:
        fail("Hist decay: new sample weight")
    h.add([0.9], [0.9], t=12.0 + 2.0 * 40)          # forces a renormalisation
    if abs(h.value(3, 3, now=92.0) - 1.0) > 1e-9 or h.value(0, 0, now=92.0) > 1e-15 or h.t0 != 92.0:
        fail("Hist renormalisation: %r %r %r" % (h.value(3, 3, now=92.0), h.value(0, 0, now=92.0), h.t0))
    h = Hist(10, 10, 0.0, 10.0, 0.0, 10.0)
    h.add_pieces([([0.5, 0.5], [0.5, 9.5])], waveforms=1)       # a vertical edge fills its column
    if [h.value(0, j) for j in range(10)] != [1.0] * 10 or sum(h.cells) != 10 or h.waveforms != 1:
        fail("Hist.add_pieces vertical edge: %s" % [h.value(0, j) for j in range(10)])
    h.clear()
    if sum(h.cells) != 0 or h.waveforms != 0:
        fail("Hist clear")

    # geometry: a V/div change or a timebase change invalidates the picture
    r1 = synth_record(codes)
    r2 = synth_record(codes, vdiv=4)
    if geometry_key(r1) == geometry_key(r2):
        fail("geometry_key ignores V/div")
    if geometry_key(r1, {"tb.code": 10}) == geometry_key(r1, {"tb.code": 11}):
        fail("geometry_key ignores tb.code")
    if geometry_key(r1, {"tb.code": 10}) != geometry_key(synth_record(list(codes)), {"tb.code": 10}):
        fail("geometry_key depends on the samples")

    # keys
    p = {"view.run": 1, "ch2.enabled": 1}
    want = {("up", 0): "set ch1.vdiv +=1", ("down", 1): "set ch2.vdiv -=1", ("right", 0): "set tb.code +=1",
            ("left", 0): "set tb.code -=1", (" ", 0): "set view.run off", ("e", 1): "set ch2.enabled off",
            ("a", 0): "ui press AUTO", ("p", 0): None}
    for (k, act), cmd in want.items():
        if key_command(k, act, p) != cmd:
            fail("key %r -> %r, expected %r" % (k, key_command(k, act, p), cmd))


def demo_stream(n_records=12, seed=3):
    """Bytes of a synthetic session: HELLO, PROPDESC, PROPS and noisy 100 kHz
    squares (both channels, CH2 inverted), triggered but not time-ordered."""
    rnd = random.Random(seed)
    names = [(0x0101, "ch1.enabled"), (0x0102, "ch1.vdiv"), (0x0201, "ch2.enabled"), (0x0301, "tb.code"),
             (0x0402, "trig.edge"), (0x0403, "trig.source"), (0x0404, "trig.level"), (0x0507, "view.run")]
    out = encode_frame(T_HELLO, 0, encode_payload({"type": T_HELLO, "proto_major": 1, "proto_minor": 0, "caps": 7,
                                                   "on": 7, "max_payload": 4096, "rec_samples": 1024,
                                                   "n_props": len(names), "table_crc": 1, "fw": "demo"}))
    seq = 1
    for i, nm in names:
        out += encode_frame(T_PROPDESC, seq, encode_payload({"type": T_PROPDESC, "id": i, "type_": 1, "unit": 0,
                                                             "decimals": 0, "group": 0, "flags": 0, "min": 0,
                                                             "max": 9, "def": 0, "name": nm, "unit_sym": ""}))
        seq += 1
    vals = {0x0101: 1, 0x0102: 3, 0x0201: 1, 0x0301: 10, 0x0402: 0, 0x0403: 0, 0x0404: 0, 0x0507: 1}
    out += encode_frame(T_PROPS, seq, encode_payload({"type": T_PROPS, "table_crc": 1, "epochs": [0] * 12,
                                                      "props": sorted(vals.items())}))
    seq += 1
    for r in range(n_records):
        ph = rnd.randrange(50)
        c1 = [max(0, min(255, int((150 if ((i + ph) % 50) < 25 else 50) + rnd.gauss(0, 3)
                               + (12 if ((i + ph) % 50) in (0, 25) else 0)))) for i in range(1024)]
        c2 = [200 - v for v in c1]
        rec = synth_record(c1, c2)
        rec["gen"] = rec["gen_end"] = 2 * (r + 1)
        out += encode_frame(T_RECORD, seq, encode_payload(rec))
        seq += 1
    return out


def persist_demo(png, fold=True, persist="inf"):
    """Render the demo session with persistence (and folding) to a PNG."""
    dm, st = Demuxer(), Stats()
    p = Plot("Agg", persist=persist, fold=fold)
    t = 0.0
    for e in dm.feed(demo_stream(), final=True):
        d = st.event(e)
        if d and d["type"] == T_PROPS:
            p.set_props(st.props_by_name())
        if d and d["type"] == T_RECORD:
            t += 0.22
            p.update(d, t)
    p.fig.savefig(png, facecolor=SURFACE)
    return p


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
        with tempfile.TemporaryDirectory() as d:
            pd = persist_demo(os.path.join(d, "persist.png"))
            if not pd.hists or min(h.waveforms for h in pd.hists.values()) < 12 * 15:
                fail("persist demo: folded waveforms %s" % [h.waveforms for h in pd.hists.values()])
            sent = []
            pd.send = sent.append

            class Ev:
                def __init__(self, k):
                    self.key = k
            for k in ("2", "up", "left", " ", "a", "1", "e", "f", "p", "c"):
                pd._on_key(Ev(k))
            if sent != ["set ch2.vdiv +=1", "set tb.code -=1", "set view.run off", "ui press AUTO",
                        "set ch1.enabled off"]:
                fail("plot keys sent %s" % sent)
            if pd.fold or pd.persist_on or pd.hists:
                fail("plot keys f/p/c did not toggle and clear")
            n0 = pd.fig.number
            pd.set_props({"ch1.vdiv": 4, "view.run": 0})       # no record: the window still follows
            if "STOP" not in pd.fig._suptitle.get_text() or pd.fig.number != n0:
                fail("PROPS without a record did not redraw: %r" % pd.fig._suptitle.get_text())
            pd.reply("> set view.run off\r\nOK view.run=off\r\n> ")
            if "OK view.run=off" not in pd.status.get_text():
                fail("reply line not shown: %r" % pd.status.get_text())
            persist_demo(os.path.join(d, "p2.png"), fold=False, persist=2.0)
        p = Plot("Agg")
        p.update(rec, 0.0)
        p.update(decode(T_RECORD, [f for f in frames if f[1] == T_RECORD][1][4]), 0.2)
        with tempfile.TemporaryDirectory() as d:
            p.fig.savefig(os.path.join(d, "plot.png"))

    # 8. the host encoder rebuilds every frame of the C vectors byte for byte
    o = 0
    for k, fr in enumerate(frames):
        b = encode_frame(fr[1], fr[2], fr[4], fr[3])
        if vec[offs[k]:offs[k] + len(b)] != b:
            fail("encode_frame differs from the C encoder on frame %d (type %d)" % (k, fr[1]))
    for k, fr in enumerate(frames):
        if fr[1] in (T_HELLO, T_PROPDESC, T_PROPS, T_RECORD, T_METER, T_STATS):
            if encode_payload(decode(fr[1], fr[4])) != fr[4]:
                fail("encode_payload(decode(x)) != x for type %d" % fr[1])
    st3 = Stats()
    rep_frame = encode_frame(T_HELLO, 9, frames[0][4], FLAG_REPLAY)
    for e in collect([rep_frame + vec]):
        st3.event(e)
    if st3.replayed != 1 or st3.seq_gaps != 0:
        fail("replayed frame: replayed=%d gaps=%d (expected 1, 0)" % (st3.replayed, st3.seq_gaps))

    # 9. the numeric core on synthetic records
    numeric_selftest(fail)

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
    ap.add_argument("--persist", nargs="?", const="inf", metavar="SECONDS|inf",
                    help="colour-graded persistence: decay time in seconds, or inf (key p toggles)")
    ap.add_argument("--fold", action="store_true", help="fold each record at its trigger crossings (key f)")
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
