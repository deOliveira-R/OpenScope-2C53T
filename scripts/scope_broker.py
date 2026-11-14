#!/usr/bin/env python3
"""Share one OpenScope between several programs (docs/stream_protocol.md).

A serial port serves one program. The broker owns it and listens on
127.0.0.1 only. Each client connection carries what the serial port carries,
shell text and binary stream frames, so a tool that talks to the scope can
talk to the broker instead:

    python3 scripts/scope_broker.py --port /dev/cu.usbmodemXXXX        # 127.0.0.1:7321
    python3 scripts/scope_stream.py --port socket://127.0.0.1:7321 --plot
    nc 127.0.0.1 7321                                                    # a plain shell

What a client sends, line by line (ended by CR, LF or CRLF):
  - a shell command: queued; the broker runs commands one at a time and
    returns each reply (the device's echo, output and "> " prompt) only to
    the client that sent it, in the order that client sent them;
  - `stream on [scope] [meter] [props] [fast]` / `stream off`: a
    subscription, not a device command. The device stream runs (scope, meter
    and props, `fast` if any subscriber asked for it) while at least one
    client subscribes; each client gets the frame types it asked for, plus
    HELLO and STATS. A late joiner first gets HELLO, PROPDESC and PROPS from
    the broker's cache, marked with header flag 0x01 (replayed). A client
    that asked for fewer than all three gets its frames with flag 0x02
    (filtered: its seq has gaps by design; the broker counts real gaps from
    the device in `!broker status`);
  - `!broker status` | `!broker release <seconds>` | `!broker quit`.

Rules:
  - A reply ends at the device's prompt: "\\n> " at the end of the text,
    then 30 ms without more text. A command whose prompt does not come
    within --cmd-timeout seconds (default 10) gets "broker: TIMEOUT ..." and
    a prompt; whatever it prints later is sent to every client as
    unsolicited text.
  - Text that arrives while no command runs (boot banner, late output) and
    the broker's own notices ("broker: ...") go to every client.
  - The device may disappear (unplug, reflash, reboot): clients stay
    connected and are told; the broker reopens the port every 0.5 s and
    restores the stream for the subscribers. A command running when it
    vanished, or sent while it is away, is answered "broker: device ...".
  - `release <s>` stops the stream, closes the port for s seconds (for a
    flashing tool), then reopens it and restores the subscriptions.
  - A client that disconnects loses its queued commands and its
    subscription; a command of its that is already running completes on the
    device and the reply is discarded.
  - Frames to a client that does not read are dropped (counted) once 1 MB is
    queued for it; text is never dropped.

Standard library only, plus pyserial for a real port (socket:// URLs, used
by the self-test's fake device, need nothing).
"""
import argparse
import collections
import os
import queue
import socket
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scope_stream as ss  # noqa: E402  (demux, frame codec)

DEFAULT_PORT = 7321
PROMPT = b"\n> "
QUIET_S = 0.03
REOPEN_S = 0.5
MAX_FRAME_QUEUE = 1 << 20
CATEGORY = {ss.T_RECORD: "scope", ss.T_METER: "meter", ss.T_PROPS: "props", ss.T_PROPDESC: "props"}
ALL_WANTS = ("scope", "meter", "props")


# ── device port ─────────────────────────────────────────────────────────────
class SocketPort:
    """socket://host:port with the slice of the pyserial API the broker uses."""

    def __init__(self, url, timeout=0.05):
        hp = url[len("socket://"):].split("/")[0]
        host, port = hp.rsplit(":", 1)
        self.sock = socket.create_connection((host, int(port)), timeout=2.0)
        self.sock.settimeout(timeout)

    def read(self, n):
        try:
            b = self.sock.recv(n)
        except socket.timeout:
            return b""
        if not b:
            raise OSError("socket closed by the device side")
        return b

    def write(self, data):
        self.sock.sendall(data)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def open_device(url, pyserial=False):
    if url.startswith("socket://") and not pyserial:
        return SocketPort(url)
    import serial  # pyserial, only for a real port
    s = serial.serial_for_url(url, baudrate=115200, timeout=0.05, write_timeout=2.0)
    return s


# ── clients ─────────────────────────────────────────────────────────────────
class Client:
    _next = 1

    def __init__(self, sock, addr, broker):
        self.id = Client._next
        Client._next += 1
        self.sock, self.addr, self.broker = sock, addr, broker
        self.alive = True
        self.wants = None          # None = not subscribed, else a set of categories
        self.fast = False
        self.cond = threading.Condition()
        self.outq = collections.deque()   # (is_frame, bytes)
        self.frame_bytes = 0
        self.frames_sent = self.frames_dropped = self.commands = 0

    def send_text(self, b):
        if b:
            self._put(False, bytes(b))

    def send_frame(self, b):
        with self.cond:
            if self.frame_bytes + len(b) > MAX_FRAME_QUEUE:
                self.frames_dropped += 1
                return
            self.frame_bytes += len(b)
        self._put(True, b)
        self.frames_sent += 1

    def _put(self, is_frame, b):
        with self.cond:
            if not self.alive:
                return
            self.outq.append((is_frame, b))
            self.cond.notify()

    def writer(self):
        while True:
            with self.cond:
                while self.alive and not self.outq:
                    self.cond.wait(0.5)
                if not self.alive and not self.outq:
                    return
                is_frame, b = self.outq.popleft()
                if is_frame:
                    self.frame_bytes -= len(b)
            try:
                self.sock.sendall(b)
            except OSError:
                self.broker.drop_client(self)
                return

    def close(self):
        with self.cond:
            self.alive = False
            self.cond.notify()
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            self.sock.close()
        except OSError:
            pass


class Job:
    """One line from a client, or an internal device command (client None)."""

    def __init__(self, client, line, kind="cmd", arg=None):
        self.client, self.line, self.kind, self.arg = client, line, kind, arg


class Inflight:
    def __init__(self, job):
        self.job = job
        self.buf = bytearray()
        self.last_rx = time.monotonic()
        self.lost = None


# ── the broker ──────────────────────────────────────────────────────────────
class Broker:
    def __init__(self, url, listen_port=DEFAULT_PORT, cmd_timeout=10.0, log=None, pyserial=False):
        self.url = url
        self.pyserial = pyserial
        self.cmd_timeout = cmd_timeout
        self.log = log or (lambda m: None)
        self.lock = threading.RLock()
        self.clients = []
        self.jobs = queue.Queue()
        self.dev = None
        self.dev_state = "closed"          # closed | open | released
        self.release_until = 0.0
        self.dev_stream = None             # None = off, else the fast flag it runs with
        self.inflight = None
        self.wake = threading.Condition(self.lock)
        self.cache = None                  # {"hello": frame, "desc": {id: frame}, "props": frame}
        self.dm = ss.Demuxer()
        self.last_seq = None
        self.stopping = False
        self.counters = collections.Counter()
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", listen_port))
        self.srv.listen(8)
        self.port = self.srv.getsockname()[1]
        self.threads = []

    # -- lifecycle --
    def start(self):
        for fn in (self._accept_loop, self._device_loop, self._exec_loop):
            t = threading.Thread(target=fn, daemon=True)
            t.start()
            self.threads.append(t)
        return self

    def stop(self):
        self.stopping = True
        try:
            self.srv.close()
        except OSError:
            pass
        with self.lock:
            for c in list(self.clients):
                c.close()
            self.clients = []
            self._close_device()
            self.wake.notify_all()
        self.jobs.put(None)

    def serve_forever(self):
        self.start()
        try:
            while not self.stopping:
                time.sleep(0.2)
        except KeyboardInterrupt:
            # as `!broker quit`: the device stream off first, then close
            self.notice("quitting (Ctrl-C)")
            self.jobs.put(Job(None, None, "quit"))
            t = time.monotonic() + 2.0
            while not self.stopping and time.monotonic() < t:
                time.sleep(0.05)
        self.stop()

    # -- notices --
    def notice(self, text, to=None):
        b = ("broker: %s\r\n" % text).encode()
        with self.lock:
            targets = [to] if to else list(self.clients)
        for c in targets:
            c.send_text(b)
        self.log(text)

    # -- clients --
    def _accept_loop(self):
        while not self.stopping:
            try:
                sock, addr = self.srv.accept()
            except OSError:
                return
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            c = Client(sock, addr, self)
            with self.lock:
                self.clients.append(c)
            threading.Thread(target=c.writer, daemon=True).start()
            threading.Thread(target=self._client_reader, args=(c,), daemon=True).start()
            self.counters["clients_total"] += 1
            self.notice("client %d connected; device %s%s" % (c.id, self.dev_state,
                        "" if self.dev_state == "open" else " (commands answer 'device not connected')"), to=c)

    def drop_client(self, c):
        with self.lock:
            if c not in self.clients:
                return
            self.clients.remove(c)
            had = c.wants is not None
            c.wants = None
        c.close()
        self.log("client %d gone" % c.id)
        if had:
            self._subscriptions_changed()

    def _client_reader(self, c):
        buf = b""
        while c.alive:
            try:
                b = c.sock.recv(4096)
            except OSError:
                b = b""
            if not b:
                break
            buf += b
            while True:
                i = min([k for k in (buf.find(b"\r"), buf.find(b"\n")) if k >= 0] or [-1])
                if i < 0:
                    break
                line, sep = buf[:i], buf[i:i + 1]
                buf = buf[i + 1:]
                if sep == b"\r" and buf.startswith(b"\n"):
                    buf = buf[1:]                   # CRLF is one line end
                self._client_line(c, line.decode("utf-8", "replace").strip())
        self.drop_client(c)

    def _client_line(self, c, line):
        if line.startswith("!broker"):
            args = line.split()[1:]
            if args[:1] == ["status"] or not args:
                self._status(c)
            elif args[:1] == ["quit"]:
                self.notice("quitting (asked by client %d)" % c.id)
                self.jobs.put(Job(c, line, "quit"))
            elif args[:1] == ["release"] and len(args) == 2:
                try:
                    s = float(args[1])
                except ValueError:
                    s = -1
                if 0 < s <= 600:
                    self.jobs.put(Job(c, line, "release", s))
                else:
                    c.send_text(b"broker: release <seconds> (0 < s <= 600)\r\n> ")
            else:
                c.send_text(b"broker: !broker status | release <seconds> | quit\r\n> ")
            return
        c.commands += 1
        self.jobs.put(Job(c, line))

    def _status(self, c):
        with self.lock:
            subs = [x for x in self.clients if x.wants is not None]
            lines = ["broker: device=%s url=%s stream=%s subscribers=%d clients=%d" % (
                self.dev_state, self.url, "off" if self.dev_stream is None else ("on fast" if self.dev_stream else "on"),
                len(subs), len(self.clients))]
            k = self.counters
            lines.append("broker: commands=%d timeouts=%d lost=%d reconnects=%d frames_in=%d bytes_in=%d "
                         "seq_gaps=%d bad_frames=%d queued=%d" % (
                             k["commands"], k["timeouts"], k["lost"], k["reconnects"], k["frames_in"], k["bytes_in"],
                             k["seq_gaps"], k["frames_bad"], self.jobs.qsize()))
            for x in self.clients:
                lines.append("broker: client %d %s:%d wants=%s sent=%d dropped=%d commands=%d%s" % (
                    x.id, x.addr[0], x.addr[1], ",".join(sorted(x.wants)) if x.wants is not None else "-",
                    x.frames_sent, x.frames_dropped, x.commands, " (you)" if x is c else ""))
        c.send_text(("\r\n".join(lines) + "\r\n> ").encode())

    # -- subscriptions --
    def _subscriptions_changed(self):
        """Queue the device stream command that matches the subscribers."""
        with self.lock:
            subs = [x for x in self.clients if x.wants is not None]
            want = None if not subs else any(x.fast for x in subs)
        self.jobs.put(Job(None, None, "sync", want))

    def _sync_stream(self, want):
        with self.lock:
            if self.dev_state != "open":
                return
            subs = [x for x in self.clients if x.wants is not None]
            want = None if not subs else any(x.fast for x in subs)   # the latest, not the queued one
            if want == self.dev_stream:
                return
        if want is None:
            self._device_command(Job(None, "stream off"))
            with self.lock:
                self.dev_stream = None
                self.cache = None
        else:
            self._device_command(Job(None, "stream on scope meter props" + (" fast" if want else "")))
            with self.lock:
                self.dev_stream = want

    def _replay_to(self, c):
        """Cached HELLO, PROPDESC, PROPS for a late joiner, flagged replayed."""
        with self.lock:
            if not self.cache or self.cache.get("hello") is None:
                return
            frames = [self.cache["hello"]] + list(self.cache["desc"].values())
            if self.cache.get("props") is not None:
                frames.append(self.cache["props"])
            for t, seq, flags, payload in frames:
                if t == ss.T_HELLO or c.wants is None or CATEGORY.get(t) in c.wants:
                    c.send_frame(ss.encode_frame(t, seq, payload, flags | ss.FLAG_REPLAY))

    # -- device side --
    def _close_device(self):
        if self.dev is not None:
            try:
                self.dev.close()
            except Exception:
                pass
        self.dev = None

    def _device_lost(self, why):
        with self.lock:
            if self.dev_state != "open":
                return
            self._close_device()
            self.dev_state = "closed"
            self.dev_stream = None
            self.cache = None
            self.dm = ss.Demuxer()
            self.counters["lost"] += 1
            if self.inflight is not None:
                self.inflight.lost = why
            self.wake.notify_all()
        self.notice("device lost (%s); reconnecting every %.1f s" % (why, REOPEN_S))

    def _device_loop(self):
        while not self.stopping:
            with self.lock:
                state, dev = self.dev_state, self.dev
            if state == "released" and time.monotonic() < self.release_until:
                time.sleep(0.05)
                continue
            if state in ("closed", "released"):
                try:
                    d = open_device(self.url, self.pyserial)
                except Exception as e:  # not there (yet)
                    self.counters["open_fail"] += 1
                    if state == "released" and self.counters["open_fail"] % 20 == 1:
                        self.log("reopen failed: %s" % e)
                    time.sleep(REOPEN_S)
                    continue
                with self.lock:
                    self.dev, self.dev_state = d, "open"
                    self.dm = ss.Demuxer()
                    self.last_seq = None
                    self.counters["reconnects"] += 1
                self.notice("device connected (%s)" % self.url)
                self.jobs.put(Job(None, None, "attach"))
                continue
            try:
                data = dev.read(4096)
            except Exception as e:
                self._device_lost(str(e) or type(e).__name__)
                continue
            if data:
                self.counters["bytes_in"] += len(data)
                for ev in self.dm.feed(data):
                    self._device_event(ev)

    def _device_event(self, ev):
        if ev[0] == "text":
            with self.lock:
                inf = self.inflight
                if inf is not None and not inf.lost:
                    inf.buf += ev[1]
                    inf.last_rx = time.monotonic()
                    self.wake.notify_all()
                    return
                targets = list(self.clients)
            for c in targets:
                c.send_text(ev[1])
            return
        if ev[0] == "bad":
            self.counters["frames_bad"] += 1
            return
        _, t, seq, flags, payload = ev
        self.counters["frames_in"] += 1
        fr = (t, seq, flags, payload)
        with self.lock:
            if self.last_seq is not None and t != ss.T_HELLO:
                self.counters["seq_gaps"] += (seq - self.last_seq - 1) & 0xFFFF
            self.last_seq = seq
            if t == ss.T_HELLO:
                self.cache = {"hello": fr, "desc": collections.OrderedDict(), "props": None}
            elif self.cache is not None:
                if t == ss.T_PROPDESC and len(payload) >= 2:
                    self.cache["desc"][payload[0] | (payload[1] << 8)] = fr
                elif t == ss.T_PROPS:
                    self.cache["props"] = fr
            b = ss.encode_frame(t, seq, payload, flags)
            bf = None
            cat = CATEGORY.get(t)
            for c in self.clients:
                if c.wants is not None and (cat is None or cat in c.wants):
                    if len(c.wants) == len(ALL_WANTS):
                        c.send_frame(b)
                    else:                   # this client skips some types: its seq has gaps
                        bf = bf or ss.encode_frame(t, seq, payload, flags | ss.FLAG_FILTERED)
                        c.send_frame(bf)

    def _device_command(self, job):
        """Run one line on the device; returns (reply bytes, outcome)."""
        with self.lock:
            if self.dev_state != "open":
                return b"", "absent"
            self.inflight = inf = Inflight(job)
            dev = self.dev
        try:
            dev.write(job.line.encode() + b"\r")
        except Exception as e:
            with self.lock:
                self.inflight = None
            self._device_lost(str(e) or type(e).__name__)
            return b"", "lost"
        self.counters["commands"] += 1
        t_end = time.monotonic() + self.cmd_timeout
        with self.lock:
            while True:
                now = time.monotonic()
                if inf.lost:
                    outcome = "lost"
                    break
                if inf.buf.endswith(PROMPT) and now - inf.last_rx >= QUIET_S:
                    outcome = "ok"
                    break
                if now >= t_end:
                    outcome = "timeout"
                    self.counters["timeouts"] += 1
                    break
                if self.stopping:
                    outcome = "lost"
                    break
                self.wake.wait(min(QUIET_S, max(0.001, t_end - now)))
            self.inflight = None
        return bytes(inf.buf), outcome

    # -- the executor: one job at a time, in arrival order --
    def _exec_loop(self):
        while not self.stopping:
            job = self.jobs.get()
            if job is None:
                return
            c = job.client
            if c is not None and not c.alive:
                continue
            try:
                self._run_job(job)
            except Exception as e:  # keep serving
                self.log("job %r failed: %r" % (job.line, e))

    def _run_job(self, job):
        c = job.client
        if job.kind == "sync":
            self._sync_stream(job.arg)
            return
        if job.kind == "attach":
            with self.lock:
                self.dev_stream = "unknown"
            self._sync_stream(None)
            return
        if job.kind == "release":
            self._release(c, job.arg)
            return
        if job.kind == "quit":
            with self.lock:
                for x in self.clients:
                    x.wants = None
            self._sync_stream(None)
            threading.Thread(target=self.stop, daemon=True).start()
            return
        line = job.line
        words = line.split()
        if not words:
            c.send_text(b"> ")
            return
        if words[0] == "stream" and len(words) >= 2 and words[1] in ("on", "off"):
            self._subscribe(c, words[1] == "on", words[2:])
            return
        reply, outcome = self._device_command(job)
        if not c.alive:
            return
        if outcome == "ok":
            c.send_text(reply)
        elif outcome == "absent":
            c.send_text(("broker: device not connected; '%s' was not sent\r\n> " % line).encode())
        elif outcome == "lost":
            c.send_text(reply + ("\r\nbroker: device lost while running '%s'\r\n> " % line).encode())
        else:
            c.send_text(reply + ("\r\nbroker: TIMEOUT: no prompt %.1f s after '%s'; later output goes to every "
                                 "client\r\n> " % (self.cmd_timeout, line)).encode())

    def _subscribe(self, c, on, opts):
        if on:
            wants, fast = set(), False
            for o in opts:
                if o in ALL_WANTS:
                    wants.add(o)
                elif o == "all":
                    wants.update(ALL_WANTS)
                elif o == "fast":
                    fast = True
                else:
                    c.send_text(("stream: unknown '%s' (scope meter props all fast)\r\n> " % o).encode())
                    return
            # one hold of the lock: no live frame can reach this client
            # before its reply and the replayed HELLO / PROPDESC / PROPS
            with self.lock:
                late = self.dev_stream not in (None, "unknown") and self.cache is not None
                c.wants, c.fast = (wants or set(ALL_WANTS)), fast
                n = sum(1 for x in self.clients if x.wants is not None)
                c.send_text(("stream: on %s%s (via broker, %d subscriber%s)\r\n> " % (
                    " ".join(sorted(c.wants)), " fast" if fast else "", n, "" if n == 1 else "s")).encode())
                if late:
                    self._replay_to(c)
        else:
            with self.lock:
                c.wants = None
                n = sum(1 for x in self.clients if x.wants is not None)
            c.send_text(("stream: off (via broker; %d other subscriber%s)\r\n> " % (n, "" if n == 1 else "s")).encode())
        self._sync_stream(None)

    def _release(self, c, seconds):
        with self.lock:
            was = self.dev_state
        if was == "open":
            with self.lock:
                had = self.dev_stream
            if had is not None:
                self._device_command(Job(None, "stream off"))
        with self.lock:
            self._close_device()
            self.dev_state = "released"
            self.dev_stream = None
            self.cache = None
            self.release_until = time.monotonic() + seconds
        if c is not None and c.alive:
            c.send_text(("broker: port released for %g s\r\n> " % seconds).encode())
        self.notice("port %s released for %g s (a flashing tool may use it); reopening after" % (self.url, seconds))


# ── self-test: a fake device on a socket ────────────────────────────────────
class FakeDevice:
    """Enough of the shell for the broker: echo, prompt after each line end,
    a few commands, and frames between commands while streaming.

      version | echo X | slow | hang (no prompt) | die (drop the connection,
      refuse for 0.6 s) | stream on ... | stream off | stream status
    """

    def __init__(self):
        self.srv = None
        self.port = None
        self.lines = []            # every command line received, in order
        self.conns = 0
        self.lock = threading.Lock()
        self.streaming = False
        self.seq = 0
        self.gen = 0
        self.stop_flag = False
        self.down_until = 0.0
        self._listen(0)
        threading.Thread(target=self._serve, daemon=True).start()

    def _listen(self, port):
        s = socket.socket()
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("127.0.0.1", port))
        s.listen(1)
        s.settimeout(0.1)
        self.srv, self.port = s, s.getsockname()[1]

    def _frame(self, t, payload):
        b = ss.encode_frame(t, self.seq, payload)
        self.seq += 1
        return b

    def _serve(self):
        while not self.stop_flag:
            if time.monotonic() < self.down_until:
                time.sleep(0.02)
                continue
            if self.srv is None:
                self._listen(self.port)
            try:
                conn, _ = self.srv.accept()
            except socket.timeout:
                continue
            except OSError:
                continue
            self.conns += 1
            self._session(conn)

    def _vanish(self, conn, seconds=0.6):
        conn.close()
        self.srv.close()
        self.srv = None
        self.streaming = False
        self.down_until = time.monotonic() + seconds

    def _session(self, conn):
        conn.settimeout(0.02)
        buf = b""
        hang = False
        next_rec = 0.0
        while not self.stop_flag:
            try:
                b = conn.recv(4096)
                if not b:
                    conn.close()
                    self.streaming = False
                    return
                buf += b
            except socket.timeout:
                pass
            except OSError:
                return
            out = b""
            while buf:
                ch, buf = buf[:1], buf[1:]
                if ch not in (b"\r", b"\n"):
                    out += ch                       # echo
                    self._cur = getattr(self, "_cur", b"") + ch
                    continue
                out += b"\r\n"
                line = getattr(self, "_cur", b"").decode()
                self._cur = b""
                hang = False
                if line:
                    with self.lock:
                        self.lines.append(line)
                    if line == "die":
                        conn.sendall(out + b"dying\r\n")
                        self._vanish(conn)
                        return
                    if line == "slowdie":
                        conn.sendall(out + b"working\r\n")
                        time.sleep(0.1)
                        self._vanish(conn)
                        return
                    r, hang = self._run(line)
                    out += r
                    if line == "slow":
                        conn.sendall(out)
                        out = b""
                        time.sleep(0.3)
                        out += b"slow done\r\n"
                if not hang:
                    out += b"> "
                if out:
                    conn.sendall(out)
                    out = b""
                if line.startswith("stream on"):
                    conn.sendall(self._stream_start())
            if out:
                conn.sendall(out)
            if self.streaming and not hang and time.monotonic() >= next_rec:
                next_rec = time.monotonic() + 0.04
                conn.sendall(self._record() + self._meter())

    def _run(self, line):
        if line == "version":
            return b"OpenScope 2C53T\r\nBuild: fake device\r\n", False
        if line.startswith("echo "):
            return line[5:].encode() + b"\r\n", False
        if line == "hang":
            return b"hanging\r\n", True
        if line.startswith("stream on"):
            self.streaming = True
            return b"stream: on scope meter props\r\n", False
        if line == "stream off":
            self.streaming = False
            return b"stream: on=0\r\n", False
        if line.startswith("stream"):
            return ("stream: on=%d\r\n" % self.streaming).encode(), False
        if line == "slow":
            return b"", False
        return ("Unknown command: '%s'\r\n" % line).encode(), False

    def _stream_start(self):
        self.seq = (self.seq + 100) & 0xFFFF
        out = self._frame(ss.T_HELLO, ss.encode_payload({"type": ss.T_HELLO, "proto_major": 1, "proto_minor": 0,
                                                         "caps": 7, "on": 7, "max_payload": 4096,
                                                         "rec_samples": 64, "n_props": 2, "table_crc": 5,
                                                         "fw": "fake"}))
        for i, nm in ((0x0102, "ch1.vdiv"), (0x0301, "tb.code")):
            out += self._frame(ss.T_PROPDESC, ss.encode_payload({"type": ss.T_PROPDESC, "id": i, "type_": 1,
                                                                 "unit": 0, "decimals": 0, "group": 0, "flags": 0,
                                                                 "min": 0, "max": 20, "def": 0, "name": nm,
                                                                 "unit_sym": ""}))
        out += self._frame(ss.T_PROPS, ss.encode_payload({"type": ss.T_PROPS, "table_crc": 5, "epochs": [0],
                                                          "props": [(0x0102, 3), (0x0301, 10)]}))
        return out

    def _record(self):
        self.gen += 2
        rec = ss.synth_record([100 + (i % 8) for i in range(64)])
        rec["gen"] = rec["gen_end"] = self.gen
        return self._frame(ss.T_RECORD, ss.encode_payload(rec))

    def _meter(self):
        return self._frame(ss.T_METER, ss.encode_payload({
            "type": ss.T_METER, "update_count": self.gen, "t_ms": 0, "value": 1.5, "bcd": 150, "func": 1,
            "auto_fn": 0, "result_class": 1, "status_code": 0, "decimal_pos": 2, "range_index": 0, "flags": 1,
            "unit": "V", "display": "1.50", "range_label": "6V"}))

    def count(self, prefix):
        with self.lock:
            return sum(1 for l in self.lines if l.startswith(prefix))

    def close(self):
        self.stop_flag = True


class TestClient:
    """A broker client that demultiplexes what it receives."""

    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port))
        self.dm = ss.Demuxer()
        self.text = bytearray()
        self.frames = []           # (type, seq, flags)
        self.lock = threading.Lock()
        self.closed = False
        threading.Thread(target=self._rx, daemon=True).start()

    def _rx(self):
        while True:
            try:
                b = self.sock.recv(65536)
            except OSError:
                b = b""
            if not b:
                self.closed = True
                return
            with self.lock:
                for e in self.dm.feed(b):
                    if e[0] == "text":
                        self.text += e[1]
                    elif e[0] == "frame":
                        self.frames.append((e[1], e[2], e[3]))

    def send(self, line):
        self.sock.sendall(line.encode() + b"\r\n")

    def wait_text(self, needle, timeout=3.0, start=0):
        t = time.monotonic() + timeout
        while time.monotonic() < t:
            with self.lock:
                if needle.encode() in self.text[start:]:
                    return True
            time.sleep(0.01)
        return False

    def wait_frames(self, ftype, n, timeout=3.0, start=0):
        t = time.monotonic() + timeout
        while time.monotonic() < t:
            with self.lock:
                if sum(1 for f in self.frames[start:] if f[0] == ftype) >= n:
                    return True
            time.sleep(0.01)
        return False

    def mark(self):
        with self.lock:
            return len(self.text), len(self.frames)

    def close(self):
        self.sock.close()


def wait(cond, timeout=3.0):
    t = time.monotonic() + timeout
    while time.monotonic() < t:
        if cond():
            return True
        time.sleep(0.01)
    return False


def selftest():
    fails = []
    fail = fails.append
    msg = ""
    dev = FakeDevice()
    br = Broker("socket://127.0.0.1:%d" % dev.port, listen_port=0, cmd_timeout=0.6).start()
    try:
        if not wait(lambda: br.dev_state == "open" and dev.count("stream off") >= 1):
            fail("broker did not open the fake device and normalise the stream")
        a, b = TestClient(br.port), TestClient(br.port)
        if not (a.wait_text("client") and b.wait_text("client")):
            fail("no greeting")

        # 1. reply routing: interleaved commands, each reply to its sender, in order
        for k in range(5):
            a.send("echo a%d" % k)
            b.send("echo b%d" % k)
        if not (a.wait_text("a4\r\n> ") and b.wait_text("b4\r\n> ")):
            fail("replies missing")
        at, bt = bytes(a.text), bytes(b.text)
        if b"b0" in at or b"a0" in bt:
            fail("a reply went to the wrong client")
        if [at.find(b"a%d\r\n" % k) for k in range(5)] != sorted(at.find(b"a%d\r\n" % k) for k in range(5)):
            fail("client a's replies out of order")
        if at.count(b"> ") < 5:
            fail("prompts missing in a's replies")

        # 2. subscription: the first subscriber starts the device stream
        on0 = dev.count("stream on")
        a.send("stream on")
        if not a.wait_text("stream: on meter props scope (via broker, 1 subscriber)"):
            fail("stream on reply: %r" % bytes(a.text[-120:]))
        if not wait(lambda: dev.count("stream on") == on0 + 1):
            fail("device stream not started")
        if not (a.wait_frames(ss.T_HELLO, 1) and a.wait_frames(ss.T_PROPDESC, 2) and a.wait_frames(ss.T_PROPS, 1)
                and a.wait_frames(ss.T_RECORD, 3) and a.wait_frames(ss.T_METER, 2)):
            fail("subscriber a: frames missing %s" % collections.Counter(f[0] for f in a.frames))
        if b.frames:
            fail("client b got frames without subscribing")

        # 3. late joiner: cache replay first, flagged, then live; type filter
        _t, nb = b.mark()
        b.send("stream on scope")
        if not (b.wait_frames(ss.T_HELLO, 1, start=nb) and b.wait_frames(ss.T_RECORD, 3, start=nb)):
            fail("late joiner: %s" % b.frames[nb:nb + 8])
        fb = b.frames[nb:]
        if not fb or fb[0][0] != ss.T_HELLO or not fb[0][2] & ss.FLAG_REPLAY:
            fail("late joiner: first frame %r, expected a replayed HELLO" % (fb[:1],))
        if [f for f in fb[1:] if not f[2] & ss.FLAG_FILTERED]:
            fail("client b (scope only) got frames without the filtered flag")
        if [f for f in fb if f[0] == ss.T_METER]:
            fail("client b asked for scope only and got METER")
        if [f for f in fb if f[0] == ss.T_PROPDESC]:
            fail("client b asked for scope only and got PROPDESC")
        d = TestClient(br.port)
        d.wait_text("client")
        d.send("stream on props")
        if not d.wait_frames(ss.T_PROPS, 1):
            fail("late props joiner: no PROPS")
        time.sleep(0.1)
        fd = d.frames
        want = [(ss.T_HELLO, ss.FLAG_REPLAY), (ss.T_PROPDESC, ss.FLAG_REPLAY), (ss.T_PROPDESC, ss.FLAG_REPLAY),
                (ss.T_PROPS, ss.FLAG_REPLAY)]
        if [(f[0], f[2]) for f in fd[:4]] != want or [f for f in fd if f[0] in (ss.T_RECORD, ss.T_METER)]:
            fail("late props joiner got %s" % fd[:6])
        d.send("stream off")
        d.wait_text("stream: off")
        d.close()
        if dev.count("stream on") != on0 + 1:
            fail("later subscribers restarted the device stream")

        # 4. reference counting
        off0 = dev.count("stream off")
        a.send("stream off")
        if not a.wait_text("stream: off (via broker; 1 other subscriber)"):
            fail("stream off reply")
        time.sleep(0.2)
        if dev.count("stream off") != off0:
            fail("one client's stream off stopped the device stream")
        _t, nb = b.mark()
        if not b.wait_frames(ss.T_RECORD, 2, start=nb):
            fail("b stopped receiving after a left")
        _t, na = a.mark()
        time.sleep(0.15)
        if [f for f in a.frames[na:] if f[0] == ss.T_RECORD]:
            fail("a still receives records after stream off")

        # 5. timeout: a command that never prompts
        ta, _f = a.mark()
        a.send("hang")
        if not a.wait_text("broker: TIMEOUT", start=ta, timeout=3):
            fail("no TIMEOUT for a command without a prompt")
        a.send("echo after-hang")
        if not a.wait_text("after-hang\r\n> ", start=ta):
            fail("no reply after a timeout")

        # 6. a client disconnecting mid-command
        c = TestClient(br.port)
        c.wait_text("client")
        c.send("slow")
        time.sleep(0.05)
        c.close()
        ta, _f = a.mark()
        a.send("echo after-c")
        if not a.wait_text("after-c\r\n> ", start=ta):
            fail("broker stuck after a client left mid-command")
        if b"slow done" in a.text[ta:]:
            fail("the departed client's reply leaked to another client")
        if not wait(lambda: len(br.clients) == 2):
            fail("departed client still listed")

        # 7. status
        ta, _f = a.mark()
        a.send("!broker status")
        if not (a.wait_text("subscribers=1 clients=2", start=ta) and a.wait_text("seq_gaps=0 ", start=ta)):
            fail("status: %r" % bytes(a.text[ta:]))

        # 8. the device vanishing mid-command; reconnect; stream restored for b
        on1 = dev.count("stream on")
        conns = dev.conns
        ta, _f = a.mark()
        tb, _f = b.mark()
        a.send("slowdie")
        if not a.wait_text("broker: device lost while running 'slowdie'", start=ta):
            fail("no 'device lost' answer: %r" % bytes(a.text[ta:]))
        if not b.wait_text("broker: device lost", start=tb):
            fail("other clients not told the device was lost")
        a.send("version")
        if not a.wait_text("device not connected; 'version' was not sent", start=ta):
            fail("command while the device is away: %r" % bytes(a.text[ta:]))
        if not (b.wait_text("broker: device connected", start=tb, timeout=4) and wait(lambda: dev.conns > conns)):
            fail("no reconnect")
        if not wait(lambda: dev.count("stream on") == on1 + 1):
            fail("stream not restored after the reconnect")
        _t, nb = b.mark()
        if not b.wait_frames(ss.T_RECORD, 2, start=nb):
            fail("b gets no records after the reconnect")
        ta, _f = a.mark()
        a.send("version")
        if not a.wait_text("Build: fake device", start=ta):
            fail("no command after the reconnect")

        # 9. release: port closed for the time asked, then reopened and restored
        conns = dev.conns
        off1, on2 = dev.count("stream off"), dev.count("stream on")
        ta, _f = a.mark()
        t0 = time.monotonic()
        a.send("!broker release 0.5")
        if not a.wait_text("port released for 0.5 s", start=ta):
            fail("release reply")
        if not wait(lambda: dev.count("stream off") == off1 + 1):
            fail("release did not stop the stream first")
        if not wait(lambda: dev.conns > conns, timeout=4):
            fail("no reopen after release")
        elif time.monotonic() - t0 < 0.45:
            fail("reopened before the release time")
        if not wait(lambda: dev.count("stream on") == on2 + 1):
            fail("stream not restored after release")

        # 10. the same through a pyserial client, as scope_stream.py uses it
        try:
            import serial  # noqa: F401
            have_serial = True
        except ImportError:
            have_serial = False
        if have_serial:
            s = ss.open_port("socket://127.0.0.1:%d" % br.port)
            dm = ss.Demuxer()
            s.write(b"\r\nstream on\r\nversion\r\n")
            got, text, t = collections.Counter(), bytearray(), time.monotonic() + 3
            while time.monotonic() < t and not (got[ss.T_RECORD] >= 2 and b"Build: fake" in text):
                for e in dm.feed(s.read(65536)):
                    if e[0] == "frame":
                        got[e[1]] += 1
                    elif e[0] == "text":
                        text += e[1]
            s.write(b"stream off\r\n")
            s.close()
            if got[ss.T_RECORD] < 2 or b"Build: fake" not in text:
                fail("pyserial socket:// client: %s %r" % (dict(got), bytes(text[-80:])))

        # 11. quit
        a.send("!broker quit")
        if not wait(lambda: br.stopping):
            fail("quit did not stop the broker")
        # 12. after quit (the fake serves one connection): the device opened through pyserial's socket:// (the real-port code path)
        if have_serial:
            br2 = Broker("socket://127.0.0.1:%d" % dev.port, listen_port=0, cmd_timeout=0.6, pyserial=True).start()
            try:
                e = TestClient(br2.port)
                if not wait(lambda: br2.dev_state == "open", timeout=4):
                    fail("pyserial device: not opened")
                e.send("version")
                if not e.wait_text("Build: fake device"):
                    fail("pyserial device: no reply")
                te, _f = e.mark()
                e.send("die")
                if not e.wait_text("device lost", start=te, timeout=3):
                    fail("pyserial device: a closed device was not noticed: %r" % bytes(e.text[te:]))
                if not e.wait_text("device connected", start=te, timeout=5):
                    fail("pyserial device: no reconnect")
                te, _f = e.mark()
                e.send("echo again")
                if not e.wait_text("again\r\n> ", start=te):
                    fail("pyserial device: no reply after the reconnect")
                e.close()
            finally:
                br2.stop()

        msg = ("pyserial socket:// client and device checked" if have_serial
               else "pyserial not installed: socket:// client and pyserial device skipped")
    finally:
        br.stop()
        dev.close()
    print("scope_broker selftest: %s: %s" % (msg, "FAIL" if fails else "OK"))
    for m in fails:
        print("  FAIL " + m)
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="the scope: a serial port, or any pyserial URL")
    ap.add_argument("--listen", type=int, default=DEFAULT_PORT, help="TCP port on 127.0.0.1 (default %d)" % DEFAULT_PORT)
    ap.add_argument("--cmd-timeout", type=float, default=10.0, help="seconds to wait for a command's prompt")
    ap.add_argument("--quiet", action="store_true", help="no log on stderr")
    ap.add_argument("--selftest", action="store_true", help="run against a fake device, no hardware")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.port:
        ap.error("--port is needed")
    log = (lambda m: None) if a.quiet else (lambda m: print("[broker] " + m, file=sys.stderr, flush=True))
    br = Broker(a.port, a.listen, a.cmd_timeout, log)
    log("listening on 127.0.0.1:%d for %s" % (br.port, a.port))
    br.serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main() or 0)
