#!/usr/bin/env python3
"""check_adc_offset.py - the FPGA_ADC_OFFSET ratchet (OpenScope ARCH round, typed units, task C2).

A record byte is the raw ADC byte minus 28. The firmware used to apply that by hand at every site, as
`x + (int)FPGA_ADC_OFFSET` (a float -28.0f in drivers/fpga.h), and passed the offset around as a parameter;
adding it twice, or not at all, is the raw-versus-record confusion with a bug history. The offset now lives
in util/units.h (ADC_REC_OFFSET, rec_from_raw(), trig_level_rec()) and the pure modules take rec_code_t.

FPGA_ADC_OFFSET stays as a deprecated alias for the sites not converted yet (the capture paths in
drivers/fpga.c, task C3). This script counts its uses in code, per file, against a baseline that only
shrinks: a use in a new file, or more uses in a baselined file, fails. The #define itself is not a use.
Comments and string literals are ignored.

Baseline:  scripts/adc_offset_baseline.txt  "<file relative to --src>\t<count>"
           --update-baseline rewrites it; a file now below its baseline is reported (FIXED) and, with
           --strict, fails, so the baseline only ever shrinks.
Exit: 0 ok, 1 new uses (or, with --strict, a stale baseline), 2 usage.
"""
import argparse
import os
import re
import sys
import tempfile

NAME = 'FPGA_ADC_OFFSET'
STRIP = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.S)
USE = re.compile(r'\b' + NAME + r'\b')
DEFINE = re.compile(r'^[ \t]*#[ \t]*define[ \t]+' + NAME + r'\b[^\n]*', re.M)


def count_uses(text):
    code = STRIP.sub(' ', text)
    code = DEFINE.sub(' ', code)
    return len(USE.findall(code))


def scan(src):
    out = {}
    for root, dirs, files in os.walk(src):
        dirs[:] = sorted(d for d in dirs if d != 'attic')
        for f in sorted(files):
            if not f.endswith(('.c', '.h')):
                continue
            path = os.path.join(root, f)
            with open(path, encoding='utf-8', errors='replace') as fh:
                n = count_uses(fh.read())
            if n:
                out[os.path.relpath(path, src).replace(os.sep, '/')] = n
    return out


def read_baseline(path):
    base = {}
    if os.path.exists(path):
        for line in open(path):
            line = line.strip()
            if line and not line.startswith('#'):
                rel, n = line.split('\t')
                base[rel] = int(n)
    return base


def check(src, baseline, update, strict, quiet):
    say = (lambda *a: None) if quiet else print
    found = scan(src)
    if update:
        with open(baseline, 'w') as fh:
            for rel in sorted(found):
                fh.write('%s\t%d\n' % (rel, found[rel]))
        say('baseline: %d files, %d uses' % (len(found), sum(found.values())))
        return 0
    base = read_baseline(baseline)
    new = fixed = 0
    for rel in sorted(found):
        if found[rel] > base.get(rel, 0):
            new += 1
            say('NEW   %-28s x%d (baseline %d): use rec_from_raw() / trig_level_rec() / ADC_REC_OFFSET (util/units.h)'
                % (rel, found[rel], base.get(rel, 0)))
    for rel in sorted(base):
        if found.get(rel, 0) < base[rel]:
            fixed += 1
            say('FIXED %-28s baseline %d now %d: run --update-baseline' % (rel, base[rel], found.get(rel, 0)))
    say('adc offset: %d uses of %s in %d files, %d new, %d fixed'
        % (sum(found.values()), NAME, len(found), new, fixed))
    return 1 if new or (strict and fixed) else 0


def self_test():
    cases = [
        ('int a = (int)raw + (int)FPGA_ADC_OFFSET;', 1),
        ('#define FPGA_ADC_OFFSET (-28.0f)\n', 0),
        ('  #  define FPGA_ADC_OFFSET (-28.0f)   /* FPGA_ADC_OFFSET */\nx = FPGA_ADC_OFFSET;', 1),
        ('/* raw + FPGA_ADC_OFFSET */ int a;', 0),
        ('// FPGA_ADC_OFFSET\nint a;', 0),
        ('puts("FPGA_ADC_OFFSET");', 0),
        ('a = FPGA_ADC_OFFSET + FPGA_ADC_OFFSET;', 2),
        ('a = MY_FPGA_ADC_OFFSET; b = FPGA_ADC_OFFSET_2;', 0),
    ]
    bad = 0
    for text, want in cases:
        got = count_uses(text)
        if got != want:
            bad += 1
            print('SELF-TEST FAIL  %r: %d, want %d' % (text, got, want))
    # the baseline mechanism: a covered use passes, one more fails, one fewer is reported (strict fails)
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, 'src')
        os.makedirs(os.path.join(src, 'drivers'))
        os.makedirs(os.path.join(src, 'attic'))
        base = os.path.join(d, 'base.txt')
        f = os.path.join(src, 'drivers', 'x.c')
        open(os.path.join(src, 'attic', 'old.c'), 'w').write('a = FPGA_ADC_OFFSET;\n')
        open(f, 'w').write('a = FPGA_ADC_OFFSET;\nb = FPGA_ADC_OFFSET;\n')
        steps = [
            (lambda: check(src, base, True, False, True), 0, 'update'),
            (lambda: check(src, base, False, True, True), 0, 'covered uses pass'),
        ]
        for fn, want, what in steps:
            if fn() != want:
                bad += 1
                print('SELF-TEST FAIL  ' + what)
        if read_baseline(base) != {'drivers/x.c': 2}:
            bad += 1
            print('SELF-TEST FAIL  baseline content (attic must be skipped)')
        open(f, 'a').write('c = FPGA_ADC_OFFSET;\n')
        if check(src, base, False, False, True) != 1:
            bad += 1
            print('SELF-TEST FAIL  a new use in a baselined file did not fail')
        open(os.path.join(src, 'y.c'), 'w').write('c = FPGA_ADC_OFFSET;\n')
        open(f, 'w').write('a = FPGA_ADC_OFFSET;\nb = FPGA_ADC_OFFSET;\n')
        if check(src, base, False, False, True) != 1:
            bad += 1
            print('SELF-TEST FAIL  a use in a new file did not fail')
        os.remove(os.path.join(src, 'y.c'))
        open(f, 'w').write('a = FPGA_ADC_OFFSET;\n')
        if check(src, base, False, False, True) != 0 or check(src, base, False, True, True) != 1:
            bad += 1
            print('SELF-TEST FAIL  a fixed use: plain run must pass, --strict must fail')
    print('adc offset self-test: %d cases, %d failed' % (len(cases) + 6, bad))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--src', default='firmware/src')
    ap.add_argument('--baseline', default='scripts/adc_offset_baseline.txt')
    ap.add_argument('--update-baseline', action='store_true')
    ap.add_argument('--strict', action='store_true', help='a stale baseline entry fails too')
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    if not os.path.isdir(a.src):
        print('no such directory: ' + a.src)
        return 2
    return check(a.src, a.baseline, a.update_baseline, a.strict, False)


if __name__ == '__main__':
    sys.exit(main())
