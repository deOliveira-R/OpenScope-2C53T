#!/usr/bin/env python3
"""The shell's command table per build profile (feature manifest, task F3).

The bench shell and the meter diagnostics are features (firmware/features.mk:
benchshell, meterdebug). Their rows come from shell_bench.h and
shell_meterdebug.h and are empty when the feature is off, so which commands an
image has is decided by the preprocessor. This test asks the firmware Makefile
for the table as each feature set compiles it (`make features-shell-table`:
usb_debug.c through the preprocessor with that set's flags) and checks:

  a. profile `bench` has exactly the rows of the baseline (the table before the
     split, scripts/shell_table_baseline.txt): same names, handlers, flags and
     help text. Only the place of a row in the table may differ (dispatch is by
     longest match; the place decides where `help` prints the line);
  b. profile `full` has every command a user, the host client, the host scripts
     and the flash/recovery path need (CORE below), with the baseline's text;
  c. nothing is lost or duplicated: names are unique in every set, the rows of
     `bench` are the rows of `full` plus the rows only benchshell brings plus
     the rows only meterdebug brings, and those three do not overlap;
  d. a command that `full` does not have is not caught by a shorter row that it
     does have (it must answer "Unknown command", not run something else).

The baseline was made from the table before the split with
    make -s features-shell-table PROFILE=bench SHELL_TABLE_SRC=<old usb_debug.c>
and `test_shell_profiles.py --dump`. A deliberate change of a command (a new
row, a changed help line) is made in the fixture too: --update rewrites it
from the `bench` profile.

Usage: test_shell_profiles.py [--make make] [--update] [--dump < table.i]
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIRMWARE = ROOT / "firmware"
BASELINE = Path(__file__).resolve().parent / "shell_table_baseline.txt"

SC_EXACT = 0x01

# Present in every image, whatever its features. The first block is the
# shell's contract with the host tools and the flash path; `stream` is there
# only when the stream feature is on (it is, in both profiles tested here).
CORE = [n.strip() for n in """
help | ? | version | features | status
fwload | fwstat | fwapply | fwswap | reboot bootloader
get | set | list | describe | settings | persist
ui press | ui long | ui seq | ui screen | ui dump | btn | keys | mode
stream
cal status | cal backup | cal restore
screenshot | screen dump | screen dumpbin | usb | logo
flash jedec | flash read | flash dump
siggen | mask | can decode | can stats | can screen
meter state | meter func | meter relays
fpga scope autoset | fpga scope basecal | fpga scope selfcal | fpga cfgsettle
fpga scope range | fpga scope vdiv | fpga scope trigger | fpga interleave
""".replace("\n", "|").split("|") if n.strip()]


def core_names():
    return CORE


STR = r'"(?:[^"\\]|\\.)*"'
ROW_RE = re.compile(
    r'\{\s*(' + STR + r')\s*,\s*([^,{}]+?)\s*,\s*([^,{}]+?)\s*,\s*([^,{}]+?)\s*,\s*'
    r'((?:' + STR + r'\s*)+|[^,{}"]+?)\s*\}')


def parse_table(pp_text):
    """Rows of shell_cmds[] in preprocessed C: (name, handler, flags, help)."""
    m = re.search(r'shell_cmds\[\]\s*=\s*\{(.*?)\n\};', pp_text, re.S)
    if not m:
        raise SystemExit("no shell_cmds[] in the preprocessed source")
    rows = []
    for name, f1, f2, flags, hlp in ROW_RE.findall(m.group(1)):
        fns = [f for f in (f1, f2) if re.fullmatch(r'\w+', f) and f != "NULL"]
        if not fns:
            continue                                  # the terminator
        value = 0
        for part in flags.split("|"):
            value |= int(part.strip().rstrip("uU"), 0)
        if hlp.lstrip().startswith('"'):
            text = "".join(s[1:-1] for s in re.findall(STR, hlp))
        else:
            text = "-"                                # NULL: an alias row
        rows.append((name[1:-1], fns[0], value, text))
    return rows


def table_of(make, *args):
    cmd = [make, "--no-print-directory", "-s", "features-shell-table", *args]
    r = subprocess.run(cmd, cwd=FIRMWARE, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("%s failed:\n%s" % (" ".join(cmd), r.stderr[-2000:]))
    return parse_table(r.stdout)


def fmt(rows):
    return "".join("%s\t%s\t0x%02x\t%s\n" % r for r in rows)


def read_baseline():
    rows = []
    for line in BASELINE.read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        name, fn, flags, text = line.split("\t", 3)
        rows.append((name, fn, int(flags, 0), text))
    return rows


def dispatch(rows, line):
    """usb_debug.c dispatch_command(): the longest whole-word prefix."""
    best = None
    for row in rows:
        name = row[0]
        if not line.startswith(name):
            continue
        nxt = line[len(name):len(name) + 1]
        if nxt not in ("", " ", "\t"):
            continue
        if (row[2] & SC_EXACT) and nxt != "":
            continue
        if best is None or len(name) > len(best[0]):
            best = row
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--make", default="make")
    ap.add_argument("--update", action="store_true", help="rewrite the baseline from profile bench")
    ap.add_argument("--dump", action="store_true", help="print the table read from preprocessed C on stdin")
    a = ap.parse_args()
    if a.dump:
        sys.stdout.write(fmt(parse_table(sys.stdin.read())))
        return 0

    bench = table_of(a.make, "PROFILE=bench")
    if a.update:
        head = [l + "\n" for l in BASELINE.read_text().splitlines() if l.startswith("#")] if BASELINE.exists() else []
        BASELINE.write_text("".join(head) + fmt(sorted(bench)))
        print("baseline rewritten: %d rows" % len(bench))
        return 0
    full = table_of(a.make, "PROFILE=full")
    no_bench = table_of(a.make, "PROFILE=bench", "WITHOUT=benchshell")
    no_meter = table_of(a.make, "PROFILE=bench", "WITHOUT=meterdebug")
    base = read_baseline()
    fails = []

    def check(ok, what):
        if not ok:
            fails.append(what)

    # c. unique names in every set
    for label, rows in (("baseline", base), ("bench", bench), ("full", full),
                        ("bench without benchshell", no_bench), ("bench without meterdebug", no_meter)):
        names = [r[0] for r in rows]
        dup = sorted({n for n in names if names.count(n) > 1})
        check(not dup, "%s: duplicated rows %s" % (label, dup))
    check(len(base) > 150, "baseline: only %d rows" % len(base))

    # a. bench = baseline, row for row
    lost = sorted(set(base) - set(bench))
    extra = sorted(set(bench) - set(base))
    check(not lost, "bench: rows of the baseline missing or changed: %s" % [r[0] for r in lost])
    check(not extra, "bench: rows not in the baseline: %s" % [r[0] for r in extra])
    check(len(bench) == len(base), "bench: %d rows, baseline %d" % (len(bench), len(base)))

    # c. the partition
    only_bench = set(bench) - set(no_bench)
    only_meter = set(bench) - set(no_meter)
    check(set(no_bench) <= set(bench) and set(no_meter) <= set(bench) and set(full) <= set(bench),
          "a smaller set has a row that bench does not")
    check(not (only_bench & only_meter), "rows in both features: %s" % sorted(r[0] for r in only_bench & only_meter))
    check(set(full) == set(bench) - only_bench - only_meter,
          "full is not bench minus the benchshell rows minus the meterdebug rows")
    check(len(full) + len(only_bench) + len(only_meter) == len(bench),
          "row count: full %d + benchshell %d + meterdebug %d != bench %d"
          % (len(full), len(only_bench), len(only_meter), len(bench)))
    check(only_bench and only_meter, "a feature brings no rows")

    # b. the core commands, in full and with both features off one at a time
    for label, rows in (("full", full), ("bench without benchshell", no_bench),
                        ("bench without meterdebug", no_meter)):
        names = {r[0] for r in rows}
        missing = [n for n in core_names() if n not in names]
        check(not missing, "%s: core commands missing: %s" % (label, missing))
    for n in core_names():
        check(not any(r[0] == n for r in only_bench | only_meter), "core command %r is a feature row" % n)

    # d. a removed command is unknown, not caught by a shorter row
    for row in sorted(only_bench | only_meter):
        for line in (row[0], row[0] + " 1"):
            hit = dispatch(full, line)
            check(hit is None, "full: %r would run %r" % (line, hit[0] if hit else None))
        check(dispatch(bench, row[0]) == row or (row[2] & 0x02), "bench: %r does not reach its own row" % row[0])

    helped = lambda rows: sum(1 for r in rows if r[3] != "-")
    print("shell profiles: bench %d rows (%d help entries) = baseline; full %d rows; benchshell %d; meterdebug %d"
          % (len(bench), helped(bench), len(full), len(only_bench), len(only_meter)))
    if fails:
        for f in fails:
            print("FAIL: " + f)
        return 1
    print("shell profiles: OK (%d core commands checked in every set)" % len(core_names()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
