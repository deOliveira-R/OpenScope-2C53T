#!/usr/bin/env python3
"""ui_walk.py -- replay a UI walk over the OpenScope CDC shell and record, per step,
where the UI is and how often the screen redraws.

A step file has one step per line (blank lines and lines starting with '#' are
skipped):

    name | command ; command ; wait <seconds> ; command

Each command is any shell line (`ui seq MENU DOWN*2 RIGHT`, `set ch1.vdiv 6`,
`mode scope`, ...). `wait <seconds>` sleeps on the host (settles longer than the
firmware's `wait:<ms>` token allows). After the step's commands the script
waits --settle seconds, then records:

  * the `ui screen` line, parsed into key=value pairs;
  * the redraw rates over --window seconds from `ui dump`'s counters
    (`draws scope_full=.. scope_live=.. redraw_all=..`), per second;
  * the last answer line of each command (`ui ...` answers give their status
    line, `OK 7 events 412 ms`);
  * with --shots, a screenshot: `screenshot save`, fetched with
    scripts/gallery_get.py's framed transfer, then deleted from the device.

Output: one line per step on stdout, and with --out DIR a JSON-lines log
(walk.jsonl) plus the screenshots (<nn>_<name>.bmp).

The firmware's `ui` commands wait until the input task has handled each press
(the handled counter), so no host sleep is needed between presses; a press it
does not handle in 2 s answers TIMEOUT and the step records it.

Needs only pyserial (and only to talk to a device):

    python3 scripts/ui_walk.py scripts/ui_walks/scope_basic.txt --port /dev/ttyACM0 --out walk/ --shots
    python3 scripts/ui_walk.py scripts/ui_walks/scope_basic.txt --dry-run

--dry-run parses the step file, checks every `ui` command against the
firmware's token rules and the shell's 127-character line, and prints the
command sequence; it opens no port.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

# The shell's line buffer is 128 bytes with the terminator (usb_debug.c CMD_BUF_SIZE).
LINE_MAX = 127

# Mirrors src/model/ui_shell.c (ui_seq_next).
KEYS = ("CH1", "CH2", "MOVE", "SELECT", "TRIGGER", "PRM", "AUTO", "SAVE", "MENU",
        "UP", "DOWN", "LEFT", "RIGHT", "OK")
LONG_KEYS = {"AUTO", "CH1", "MOVE", "SELECT", "TRIGGER", "PRM", "CH2", "SAVE", "MENU", "OK"}
MAX_REPEAT = 50
MAX_WAIT_MS = 5000
PRESS_TIMEOUT_S = 2.0

DRAWS = re.compile(r"scope_full=(\d+) scope_live=(\d+) redraw_all=(\d+)")
SAVED = re.compile(r"screenshot: saved (\d+)\.bmp")


class StepError(ValueError):
    pass


# ── parsing (pure) ──────────────────────────────────────────────────────────

def parse_token(tok: str) -> Tuple[str, str, int]:
    """One `ui seq` token -> (kind, key, n): ("press", KEY, n), ("long", KEY, 1)
    or ("wait", "", ms). Raises StepError with the firmware's reason."""
    t = tok.strip()
    low = t.lower()
    if low.startswith("wait:"):
        ms = t[5:]
        if not ms.isdigit() or len(ms) > 5 or int(ms) > MAX_WAIT_MS:
            raise StepError(f"'{tok}': bad form (wait:0..{MAX_WAIT_MS})")
        return ("wait", "", int(ms))
    if low.startswith("long:"):
        key = t[5:].upper()
        if "*" in key:
            raise StepError(f"'{tok}': bad form (no repeat on a long press)")
        if key == "POWER":
            raise StepError(f"'{tok}': POWER is refused (shutdown / IAP)")
        if key not in KEYS:
            raise StepError(f"'{tok}': unknown key")
        if key not in LONG_KEYS:
            raise StepError(f"'{tok}': no long press on this key")
        return ("long", key, 1)
    key, star, n = t.partition("*")
    key = key.upper()
    if key == "POWER":
        raise StepError(f"'{tok}': POWER is refused (shutdown / IAP)")
    if key not in KEYS:
        raise StepError(f"'{tok}': unknown key")
    count = 1
    if star:
        if not n.isdigit() or len(n) > 5 or not 1 <= int(n) <= MAX_REPEAT:
            raise StepError(f"'{tok}': bad form ({key}*1..{MAX_REPEAT})")
        count = int(n)
    return ("press", key, count)


def ui_events(cmd: str) -> Optional[Tuple[int, int]]:
    """For a `ui press/long/seq` line: (key events, firmware wait ms); None for
    any other command. Raises StepError on a token the firmware would refuse."""
    words = cmd.split()
    if len(words) < 2 or words[0] != "ui":
        return None
    verb, args = words[1], words[2:]
    if verb == "seq":
        toks = args
    elif verb == "press":
        if not 1 <= len(args) <= 2:
            raise StepError(f"'{cmd}': usage: ui press KEY [1..{MAX_REPEAT}]")
        toks = [args[0] + ("*" + args[1] if len(args) == 2 else "")]
    elif verb == "long":
        if len(args) != 1:
            raise StepError(f"'{cmd}': usage: ui long KEY")
        toks = ["long:" + args[0]]
    else:
        return None                      # ui screen, ui dump: plain commands
    if not toks:
        raise StepError(f"'{cmd}': no keys given")
    events = wait_ms = 0
    for tok in toks:
        kind, _, n = parse_token(tok)
        if kind == "wait":
            wait_ms += n
        else:
            events += n
    return events, wait_ms


@dataclass
class Step:
    index: int
    name: str
    commands: List[str] = field(default_factory=list)   # "wait <s>" kept as is


def parse_steps(text: str) -> List[Step]:
    steps: List[Step] = []
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        name, bar, cmds = line.partition("|")
        name = name.strip()
        if not bar or not name or not re.fullmatch(r"[A-Za-z0-9_.-]+", name):
            raise StepError(f"line {lineno}: expected 'name | cmd ; cmd' with a plain name")
        step = Step(len(steps), name)
        for c in (x.strip() for x in cmds.split(";")):
            if not c:
                continue
            if c.startswith("wait "):
                try:
                    float(c[5:])
                except ValueError:
                    raise StepError(f"line {lineno}: bad host wait '{c}'") from None
            else:
                if len(c) > LINE_MAX:
                    raise StepError(f"line {lineno}: '{c[:30]}...' is {len(c)} characters "
                                    f"(the shell keeps {LINE_MAX})")
                try:
                    ui_events(c)
                except StepError as e:
                    raise StepError(f"line {lineno}: {e}") from None
            step.commands.append(c)
        steps.append(step)
    return steps


def parse_screen(line: str) -> Dict[str, str]:
    return dict(kv.split("=", 1) for kv in line.split() if "=" in kv)


# ── the device (pyserial, imported only here) ───────────────────────────────

class Shell:
    def __init__(self, port: str, baud: int = 115200):
        import serial                    # pyserial; not needed for --dry-run
        self.ser = serial.Serial(port, baud, timeout=0.05)
        time.sleep(0.2)
        self.ser.reset_input_buffer()

    def xact(self, line: str, maxt: float = 6.0) -> str:
        """Send one line; read until the shell's prompt ('> ') comes back, or maxt.

        The shell echoes the line before it runs the command and prints the
        prompt only after the handler returned (and every `ui` command returns
        only once its presses were handled), so the prompt is the one reliable
        end: silence is not, a slow command (a SAVE grab, a mode switch) is
        silent for seconds."""
        self.ser.reset_input_buffer()
        self.ser.write(line.encode() + b"\r\n")
        buf, t0 = b"", time.time()
        while time.time() - t0 < maxt:
            chunk = self.ser.read(4096)
            if chunk:
                buf += chunk
            elif buf.endswith(b"> "):
                break
        return buf.decode(errors="replace")

    @staticmethod
    def answer_lines(reply: str, cmd: str) -> List[str]:
        out = []
        for ln in reply.replace("\r", "").split("\n"):
            s = ln.strip()
            if s.startswith(">"):
                s = s[1:].strip()
            if s and s != cmd:
                out.append(s)
        return out

    def run(self, cmd: str, maxt: Optional[float] = None) -> List[str]:
        if maxt is None:
            ev = ui_events(cmd)
            maxt = 6.0 if ev is None else 3.0 + ev[0] * PRESS_TIMEOUT_S + ev[1] / 1000.0
        return self.answer_lines(self.xact(cmd, maxt=maxt), cmd)

    def screen(self) -> Tuple[str, Dict[str, str]]:
        for ln in self.run("ui screen"):
            if ln.startswith("screen="):
                return ln, parse_screen(ln)
        return "", {}

    def draws(self) -> Optional[Tuple[int, int, int]]:
        m = DRAWS.search("\n".join(self.run("ui dump")))
        return tuple(int(x) for x in m.groups()) if m else None

    def shot(self, path: str) -> str:
        reply = "\n".join(self.run("screenshot save", maxt=20.0))
        m = SAVED.search(reply)
        if not m:
            return "save failed: " + reply[-80:].replace("\n", " ")
        n = m.group(1)
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import gallery_get               # the framed, CRC-checked transfer
        try:
            _, data = gallery_get.get(self.ser, n)
        except SystemExit as e:           # gallery_get reports by SystemExit
            return f"get failed: {e}"
        finally:
            self.run(f"screenshot delete {n}", maxt=10.0)
        with open(path, "wb") as f:
            f.write(data)
        return os.path.basename(path)


# ── the walk ────────────────────────────────────────────────────────────────

def walk(steps: List[Step], sh: Shell, out: Optional[str], shots: bool,
         settle: float, window: float) -> int:
    log = open(os.path.join(out, "walk.jsonl"), "w") if out else None
    problems = 0
    for st in steps:
        tag = f"{st.index:02d}_{st.name}"
        answers = []
        for c in st.commands:
            if c.startswith("wait "):
                time.sleep(float(c[5:]))
                continue
            lines = sh.run(c)
            # a `ui` answer is the status line then the screen line: keep the status
            status = [ln for ln in lines if not ln.startswith("screen=")]
            ans = status[-1] if status else ""
            if ans.startswith(("TIMEOUT", "BUSY", "INVALID", "REFUSED", "ERR", "Unknown")):
                problems += 1
            answers.append(f"{c} -> {ans[:80]}")
        time.sleep(settle)
        c0, t0 = sh.draws(), time.time()
        time.sleep(window)
        c1, dt = sh.draws(), time.time() - t0
        rates = None
        if c0 and c1 and dt > 0:
            rates = {k: round((b - a) / dt, 2) for k, a, b in
                     zip(("scope_full", "scope_live", "redraw_all"), c0, c1)}
        screen_line, screen = sh.screen()
        shot = sh.shot(os.path.join(out or ".", tag + ".bmp")) if shots else None
        rec = {"step": st.index, "name": st.name, "commands": st.commands, "answers": answers,
               "screen": screen, "screen_line": screen_line, "rates_per_s": rates,
               "window_s": round(dt, 2), "shot": shot}
        if log:
            log.write(json.dumps(rec) + "\n")
            log.flush()
        r = rates or {}
        print(f"{tag}: full/s={r.get('scope_full', '?')} live/s={r.get('scope_live', '?')} "
              f"all/s={r.get('redraw_all', '?')} | {screen_line or '?'}"
              + (f" | {shot}" if shots else "")
              + (" | " + " ; ".join(answers) if answers else ""), flush=True)
    if log:
        log.close()
    return problems


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("steps", help="step file (name | cmd ; cmd ; wait <s>)")
    ap.add_argument("--port", help="CDC shell port, e.g. /dev/ttyACM0 or /dev/cu.usbmodemXXXX")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--out", help="directory for walk.jsonl and the screenshots")
    ap.add_argument("--shots", action="store_true", help="screenshot every step (gallery transfer)")
    ap.add_argument("--settle", type=float, default=2.0, help="seconds after a step's commands")
    ap.add_argument("--window", type=float, default=4.0, help="seconds the redraw rates are taken over")
    ap.add_argument("--dry-run", action="store_true", help="parse and print the commands; no port")
    a = ap.parse_args(argv)

    with open(a.steps) as f:
        try:
            steps = parse_steps(f.read())
        except StepError as e:
            print(f"{a.steps}: {e}", file=sys.stderr)
            return 2

    if a.dry_run:
        events = 0
        for st in steps:
            for c in st.commands:
                ev = None if c.startswith("wait ") else ui_events(c)
                note = f"   # {ev[0]} key events" if ev else ""
                if ev:
                    events += ev[0]
                print(f"{st.index:02d}_{st.name}: {c}{note}")
        print(f"{len(steps)} steps, {sum(len(s.commands) for s in steps)} commands, "
              f"{events} key events: all valid")
        return 0

    if not a.port:
        ap.error("--port is required (or --dry-run)")
    if a.out:
        os.makedirs(a.out, exist_ok=True)
    sh = Shell(a.port, a.baud)
    problems = walk(steps, sh, a.out, a.shots, a.settle, a.window)
    if problems:
        print(f"{problems} command(s) answered TIMEOUT/BUSY/INVALID/REFUSED/unknown", file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
