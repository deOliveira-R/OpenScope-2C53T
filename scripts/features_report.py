#!/usr/bin/env python3
"""features_report.py - what an OpenScope image contains, per feature.

Reads the linker map of the image in the build directory and the record the
build wrote next to it (features.built: the manifest of firmware/features.mk
as that build evaluated it), and attributes every input section to a feature
or to the core:

  own     the feature's own objects (NAME_srcs in features.mk)
  shared  its symbols inside shared objects (NAME_shared: `object.o:regex`,
          matched against the input section name). A lower bound: anonymous
          strings and helpers the feature alone calls are missed. The true
          cost of a feature is `make features-ab FEATURE=name`.

Columns:  flash = what is programmed (vectors, code, constants, the load
                  image of .data);  RAM = static RAM (.data, .bss, .noinit and
                  the linker's heap/stack reserve).
`unattributed` is what the output sections hold beyond the sum of the listed
input sections: alignment fill, the heap/stack reserve, and the string-merge
correction (strings are counted per object before identical ones are merged,
so that line is usually negative for flash).

The column totals must equal `arm-none-eabi-size` (flash = text + data;
RAM = bss + the .data section); if they do not, the tool exits 1.

usage:
  features_report.py --build-dir firmware/build [--size-tool arm-none-eabi-size]
                     [--fwload-max N] [--detail FEATURE]
  features_report.py --ab ON_DIR OFF_DIR --feature NAME     (A/B delta of two builds)
  features_report.py --self-test
"""
import argparse
import collections
import os
import re
import subprocess
import sys

# Output sections that occupy the device, and what they count as.
FLASH, RAM, BOTH = "flash", "ram", "both"
OUT_CLASS = {
    ".isr_vector": FLASH, ".text": FLASH, ".rodata": FLASH, ".ARM.extab": FLASH, ".ARM": FLASH,
    ".ARM.exidx": FLASH, ".preinit_array": FLASH, ".init_array": FLASH, ".fini_array": FLASH,
    ".data": BOTH, ".bss": RAM, ".noinit": RAM, "._user_heap_stack": RAM,
}
# Zero-size or non-loaded output sections that may appear between them.
OUT_IGNORE = (".vfp11_veneer", ".v4_bx", ".iplt", ".rel.dyn", ".tm_clone_table", ".igot.plt", ".spim",
              ".glue_7", ".glue_7t", ".ARM.attributes", ".comment", ".debug", ".stab", ".note")

RX_OUT = re.compile(r"^(\.[\w.]+|_[\w.]+)\s*(?:0x([0-9a-f]+)\s+0x([0-9a-f]+))?")
RX_OUT2 = re.compile(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)")
RX_IN = re.compile(r"^ (\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$")
RX_IN2 = re.compile(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$")
RX_IN_NAME = re.compile(r"^ (\S+)$")
RX_RELAX = re.compile(r"^\s+0x([0-9a-f]+) \(size before relaxing\)")
RX_MEM = re.compile(r"^(\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+\S+\s*$")


def objname(path):
    """build/foo.o -> foo.o ; .../libc.a(lib_a-memcpy.o) -> lib:libc.a"""
    m = re.search(r"([^/]+)\(([^)]+)\)$", path)
    if m:
        return "lib:" + m.group(1)
    return path.split("/")[-1]


def parse_map(text):
    """-> (out, recs, mem)
    out:  {output section: size} for the sections in OUT_CLASS
    recs: [(output section, input section, size, object)] - size is the size
          before string merging where the map gives one
    mem:  {region: (origin, length)}"""
    lines = text.split("\n")
    mem = {}
    start = 0
    for i, l in enumerate(lines):
        if l.startswith("Memory Configuration"):
            for m in lines[i + 1:i + 40]:
                if m.startswith("Linker script and memory map"):
                    break
                mm = RX_MEM.match(m)
                if mm and mm.group(1) not in ("Name", "*default*"):
                    mem[mm.group(1)] = (int(mm.group(2), 16), int(mm.group(3), 16))
        if l.startswith("Linker script and memory map"):
            start = i
            break
    out = {}
    recs = []
    cur = None          # current output section (None: one we do not count)
    want_out_size = None
    pend = None
    for l in lines[start:]:
        if want_out_size:
            m = RX_OUT2.match(l)
            if m:
                out[want_out_size] = int(m.group(2), 16)
            want_out_size = None
            if m:
                continue
        m = RX_RELAX.match(l)
        if m:
            if recs and cur and recs[-1][0] == cur:
                o, sec, _, obj = recs[-1]
                recs[-1] = (o, sec, int(m.group(1), 16), obj)
            pend = None
            continue
        if l and not l[0].isspace():
            m = RX_OUT.match(l)
            pend = None
            if not m:
                continue
            name = m.group(1)
            if name in OUT_CLASS:
                cur = name
                if m.group(3) is not None:
                    out[name] = int(m.group(3), 16)
                else:
                    want_out_size = name
            elif name.startswith(OUT_IGNORE) or name.startswith("."):
                cur = None
            # a symbol assignment (_sdata = .) at column 0 leaves cur alone
            continue
        if cur is None:
            continue
        m = RX_IN.match(l)
        if m:
            if m.group(1) != "*fill*" and int(m.group(2), 16):
                recs.append((cur, m.group(1), int(m.group(3), 16), m.group(4).strip()))
            pend = None
            continue
        if pend:
            m = RX_IN2.match(l)
            if m and int(m.group(1), 16):
                recs.append((cur, pend, int(m.group(2), 16), m.group(3).strip()))
            pend = None
            continue
        m = RX_IN_NAME.match(l)
        if m and m.group(1) != "*fill*":
            pend = m.group(1)
    recs = [r for r in recs if r[2]]
    return out, recs, mem


def parse_built(text):
    """features.built -> (profile, [feature dict])"""
    profile = "?"
    feats = []
    for l in text.split("\n"):
        f = l.split("\t")
        if f[0] == "profile" and len(f) > 1:
            profile = f[1]
        elif f[0] == "feature" and len(f) >= 7:
            shared = []
            for item in f[5].split():
                o, _, rx = item.partition(":")
                shared.append((o, re.compile(rx)))
            feats.append({"name": f[1], "on": f[2] == "1", "bit": int(f[3]), "objs": set(f[4].split()),
                          "shared": shared, "desc": f[6]})
    return profile, feats


def attribute(recs, feats):
    """-> {(owner, where): Counter(flash=, ram=)}, detail {owner: [(size, out, sec, obj, where)]}"""
    own = {}
    for f in feats:
        for o in f["objs"]:
            own[o] = f["name"]
    tot = collections.defaultdict(collections.Counter)
    detail = collections.defaultdict(list)
    for out, sec, size, path in recs:
        obj = objname(path)
        owner, where = "core", "core"
        if obj in own:
            owner, where = own[obj], "own"
        else:
            for f in feats:
                if any(o == obj and rx.search(sec) for o, rx in f["shared"]):
                    owner, where = f["name"], "shared"
                    break
        cls = OUT_CLASS[out]
        if cls in (FLASH, BOTH):
            tot[(owner, where)]["flash"] += size
        if cls in (RAM, BOTH):
            tot[(owner, where)]["ram"] += size
        detail[owner].append((size, out, sec, obj, where))
    return tot, detail


def totals(out):
    flash = sum(v for k, v in out.items() if OUT_CLASS[k] in (FLASH, BOTH))
    ram = sum(v for k, v in out.items() if OUT_CLASS[k] in (RAM, BOTH))
    return flash, ram


def run_size(tool, elf):
    """Berkeley text, data, bss of the ELF."""
    o = subprocess.run([tool, elf], capture_output=True, text=True, check=True).stdout.split("\n")
    t, d, b = o[1].split()[:3]
    return int(t), int(d), int(b)


def n(v):
    return "{:,}".format(v)


def report(map_text, built_text, size3, bin_size=None, fwload_max=None, detail_name=None, out=sys.stdout):
    """Prints the table; returns True when the totals equal `size`."""
    def p(s=""):
        out.write(s + "\n")
    sections, recs, mem = parse_map(map_text)
    profile, feats = parse_built(built_text)
    tot, detail = attribute(recs, feats)
    if detail_name:
        for size, o, sec, obj, where in sorted(detail.get(detail_name, []), reverse=True)[:60]:
            p("%8d  %-7s %-12s %-28s %s" % (size, where, o, obj, sec))
        return True
    flash_total, ram_total = totals(sections)
    p("image: profile %s, features on: %s" % (profile, " ".join(f["name"] for f in feats if f["on"]) or "(none)"))
    p("%-13s %-5s %12s %14s %10s %12s" % ("feature", "state", "flash(own)", "flash(shared)", "RAM(own)", "RAM(shared)"))
    sum_flash = sum_ram = 0
    for f in feats:
        if not f["on"]:
            p("%-13s %-5s %12s %14s %10s %12s" % (f["name"], "off", "-", "-", "-", "-"))
            continue
        o, s = tot[(f["name"], "own")], tot[(f["name"], "shared")]
        sum_flash += o["flash"] + s["flash"]
        sum_ram += o["ram"] + s["ram"]
        p("%-13s %-5s %12s %14s %10s %12s" % (f["name"], "on", n(o["flash"]), n(s["flash"]), n(o["ram"]), n(s["ram"])))
    core = tot[("core", "core")]
    sum_flash += core["flash"]
    sum_ram += core["ram"]
    p("%-13s %-5s %12s %14s %10s" % ("core", "", n(core["flash"]), "", n(core["ram"])))
    p("%-13s %-5s %12s %14s %10s   (fill, heap/stack reserve, string merging)"
      % ("unattributed", "", n(flash_total - sum_flash), "", n(ram_total - sum_ram)))
    text, data, bss = size3
    data_sec = sections.get(".data", 0)
    ok = flash_total == text + data and ram_total == bss + data_sec
    p("%-13s %-5s %12s %14s %10s   size: text %s + data %s = %s flash; bss %s + .data %s = %s RAM: %s"
      % ("TOTAL", "", n(flash_total), "", n(ram_total), n(text), n(data), n(text + data), n(bss), n(data_sec),
         n(bss + data_sec), "OK" if ok else "MISMATCH"))
    lim = []
    if bin_size is not None and fwload_max:
        lim.append("fwload ceiling %s: firmware.bin %s, %s B left" % (n(fwload_max), n(bin_size), n(fwload_max - bin_size)))
    if "RAM" in mem:
        lim.append("RAM region %s: %s B above the static RAM (the main stack lives there)"
                   % (n(mem["RAM"][1]), n(mem["RAM"][1] - ram_total)))
    for l in lim:
        p(l)
    if not ok:
        p("features_report: the map totals do not equal arm-none-eabi-size (a new output section? see OUT_CLASS)")
    return ok


def per_object(map_text):
    c = collections.defaultdict(collections.Counter)
    for out, _sec, size, path in parse_map(map_text)[1]:
        cls = OUT_CLASS[out]
        if cls in (FLASH, BOTH):
            c[objname(path)]["flash"] += size
        if cls in (RAM, BOTH):
            c[objname(path)]["ram"] += size
    return c


def ab(on_dir, off_dir, feature, size_tool, out=sys.stdout):
    def p(s=""):
        out.write(s + "\n")
    s_on = run_size(size_tool, os.path.join(on_dir, "firmware.elf"))
    s_off = run_size(size_tool, os.path.join(off_dir, "firmware.elf"))
    p("features-ab %s          text      data       bss" % feature)
    p("  with    %-10s %9s %9s %9s" % ("", n(s_on[0]), n(s_on[1]), n(s_on[2])))
    p("  without %-10s %9s %9s %9s" % ("", n(s_off[0]), n(s_off[1]), n(s_off[2])))
    p("  delta   %-10s %9s %9s %9s" % ("", n(s_on[0] - s_off[0]), n(s_on[1] - s_off[1]), n(s_on[2] - s_off[2])))
    a = per_object(open(os.path.join(on_dir, "firmware.map"), errors="replace").read())
    b = per_object(open(os.path.join(off_dir, "firmware.map"), errors="replace").read())
    rows = [(a[o]["flash"] - b[o]["flash"], a[o]["ram"] - b[o]["ram"], o) for o in set(a) | set(b)]
    rows = [r for r in rows if r[0] or r[1]]
    p("  where (per object, strings before merging):")
    for fl, ra, o in sorted(rows, reverse=True):
        p("    %-28s flash %7d  RAM %6d" % (o, fl, ra))
    p("  the feature costs %s B flash (text+data), %s B RAM (bss)"
      % (n(s_on[0] + s_on[1] - s_off[0] - s_off[1]), n(s_on[2] - s_off[2])))
    return 0


# ── self-test: a small synthetic map ────────────────────────────────────────
SELF_MAP = """Discarded input sections

 .text.dead     0x00000000       0x40 build/core.o

Memory Configuration

Name             Origin             Length             Attributes
FLASH_APP        0x08007000         0x000e9000         xr
RAM              0x20000000         0x00038000         xrw
*default*        0x00000000         0xffffffff

Linker script and memory map

.isr_vector     0x08004000       0x10
 .isr_vector    0x08004000       0x10 build/startup.o

.text           0x08007000       0x70
                0x08007000                        . = ALIGN (0x4)
 .text.main     0x08007000       0x20 build/core.o
                0x08007000                main
 .text.cmd_stream
                0x08007020       0x10 build/shell.o
 *fill*         0x08007030        0x2
 .text.stream_poll
                0x08007034       0x30 build/stream.o
 .text.memcpy   0x08007064        0xc /tc/lib/libc.a(lib_a-memcpy.o)

.rodata         0x08007070       0x30
 .rodata.str1.1
                0x08007070       0x18 build/core.o
                                 0x1c (size before relaxing)
 .rodata.str1.1
                0x08007088        0x8 build/stream.o
                                 0x14 (size before relaxing)
 .rodata.tab    0x08007090       0x10 build/stream.o

.ARM.extab
 *(.ARM.extab*)

.data           0x20000000        0x8 load address 0x080070a0
 .data.x        0x20000000        0x8 build/core.o

.bss            0x20000008       0x28
 .bss.s         0x20000008       0x1c build/stream.o
 *fill*         0x20000024        0x4
 COMMON         0x20000028        0x8 build/core.o

._user_heap_stack
                0x20000030      0x600
                0x20000030                        . = ALIGN (0x8)

.debug_info     0x00000000      0x100
 .debug_info    0x00000000      0x100 build/core.o
"""
SELF_BUILT = ("profile\tbench\n"
              "feature\tstream\t1\t0\tstream.o stream_frame.o\tshell.o:cmd_stream|stream_port_\tBinary frames\n"
              "feature\tfft\t0\t1\tfft.o\t\tFFT\n")


def self_test():
    fails = []

    def check(cond, what):
        if not cond:
            fails.append(what)

    sections, recs, mem = parse_map(SELF_MAP)
    check(sections == {".isr_vector": 0x10, ".text": 0x70, ".rodata": 0x30, ".data": 8, ".bss": 0x28,
                       "._user_heap_stack": 0x600}, "output sections: %r" % sections)
    check(mem.get("RAM") == (0x20000000, 0x38000), "memory regions: %r" % mem)
    check(not any(r[1] == ".text.dead" or r[1].startswith(".debug") for r in recs), "discarded/debug counted")
    check(objname("/tc/lib/libc.a(lib_a-memcpy.o)") == "lib:libc.a", "objname lib")
    profile, feats = parse_built(SELF_BUILT)
    check(profile == "bench" and [f["name"] for f in feats] == ["stream", "fft"] and feats[0]["on"]
          and not feats[1]["on"], "features.built parse")
    tot, _ = attribute(recs, feats)
    # stream own: text 0x30, rodata str 0x14 (before merging) + 0x10; bss 0x1c
    check(tot[("stream", "own")] == {"flash": 0x30 + 0x14 + 0x10, "ram": 0x1c}, "stream own: %r" % tot[("stream", "own")])
    check(tot[("stream", "shared")] == {"flash": 0x10}, "stream shared: %r" % tot[("stream", "shared")])
    # core: isr 0x10, main 0x20, memcpy 0xc, str 0x1c, .data 8 (flash and RAM), COMMON 8
    check(tot[("core", "core")] == {"flash": 0x10 + 0x20 + 0xc + 0x1c + 8, "ram": 8 + 8}, "core: %r" % tot[("core", "core")])
    flash_total, ram_total = totals(sections)
    check(flash_total == 0x10 + 0x70 + 0x30 + 8 and ram_total == 8 + 0x28 + 0x600, "totals")

    class Sink:
        def __init__(self):
            self.s = ""

        def write(self, x):
            self.s += x

    good = Sink()
    # size(1) would say: text = isr + text + rodata, data = .data, bss = bss + reserve
    size3 = (0x10 + 0x70 + 0x30, 8, 0x28 + 0x600)
    check(report(SELF_MAP, SELF_BUILT, size3, bin_size=200, fwload_max=1000, out=good), "report: totals refused")
    check("size: " in good.s and ": OK" in good.s and "unattributed" in good.s and "fft           off" in good.s,
          "report text:\n" + good.s)
    # the unattributed line: flash 0xc0 - (0x54 + 0x10 + 0x60) = -8 + 2 fill... computed, not copied
    attributed_flash = 0x54 + 0x10 + (0x10 + 0x20 + 0xc + 0x1c + 8)
    check(("%s" % n(flash_total - attributed_flash)) in good.s, "unattributed flash value")
    bad = Sink()
    check(not report(SELF_MAP, SELF_BUILT, (size3[0] + 4, size3[1], size3[2]), out=bad), "a text mismatch passed")
    check("MISMATCH" in bad.s, "mismatch not printed")
    bad = Sink()
    check(not report(SELF_MAP, SELF_BUILT, (size3[0], size3[1], size3[2] - 8), out=bad), "a bss mismatch passed")
    print("features_report self-test: %d checks failed%s" % (len(fails), "".join("\n  " + f for f in fails)))
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--build-dir", default="firmware/build")
    ap.add_argument("--size-tool", default="arm-none-eabi-size")
    ap.add_argument("--fwload-max", type=int, default=0, help="in-band fwload ceiling in bytes (0: not shown)")
    ap.add_argument("--detail", metavar="FEATURE", help="largest input sections attributed to FEATURE (or core)")
    ap.add_argument("--ab", nargs=2, metavar=("ON_DIR", "OFF_DIR"), help="A/B delta of two build directories")
    ap.add_argument("--feature", default="?")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    if a.ab:
        return ab(a.ab[0], a.ab[1], a.feature, a.size_tool)
    d = a.build_dir
    need = [os.path.join(d, f) for f in ("firmware.map", "firmware.elf", "features.built")]
    for f in need:
        if not os.path.exists(f):
            print("features_report: %s is missing: build an image first" % f)
            return 1
    binf = os.path.join(d, "firmware.bin")
    ok = report(open(need[0], errors="replace").read(), open(need[2]).read(), run_size(a.size_tool, need[1]),
                bin_size=os.path.getsize(binf) if os.path.exists(binf) else None, fwload_max=a.fwload_max,
                detail_name=a.detail)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
