#!/usr/bin/env python3
"""model_config.py - the property table on the host: list it, read a device's settings into a readable
profile, apply a profile to a device, compare a profile with the built-in defaults.

Two things, as in the firmware (firmware/src/model/model.h):
  built-in defaults   const, in the property table (firmware/src/model/model_table.h, the `def` column)
  user settings       what a device has saved (its settings record); they win over the defaults at boot

A PROFILE is a readable file of settings: JSON, one "name": "value" per property, the value exactly as the
shell prints and accepts it ("1.5 V", "AC", "on", "Freq,Vp-p"; an index for a measured label such as
ch1.vdiv, with the label next to it in "labels"):

    {
      "format": "openscope-profile/1",
      "name": "bench",
      "source": "device socket://127.0.0.1:7321, Build: ...",
      "settings": { "ch1.vdiv": "3", "ch1.coupling": "DC", "gen.amp": "3.0 V", ... },
      "labels":   { "ch1.vdiv": "1.00V" }
    }

Commands (run from anywhere; the table is read from this checkout's firmware source):
  list [glob]                         the table: name, type, range, built-in default, flags
  defaults [-o FILE]                  the built-in defaults of the saved (persisted) properties as a profile
                                      (profiles/default.json is this)
  from-device --port P [-o FILE]      `get *` on the device, the persisted properties into a profile
        [--session]                   ... and the session-only ones too
  to-device FILE --port P [--dry-run] `set` each setting of the profile, in table order; reports every value
                                      the device refused, clamped or stored for later, then reads all back
  diff FILE [OTHER]                   the profile against the built-in defaults (or against OTHER)
  build FILE -o HEADER                the profile as build-time defaults (make DEFAULTS=FILE; see the Makefile)
  --self-test [--c-defaults FILE] [--c-shell FILE] [--c-profile PROFILE FILE]
                                      no device: the table parser, the value text, a fake shell in-process and
                                      over a socket on an ephemeral port, every command above

--port is a serial device (/dev/cu.usbmodem..., needs pyserial) or a pyserial URL; socket://HOST:PORT
(scripts/scope_broker.py) works without pyserial.

What a profile never carries: calibration (the usercal properties il.gain / il.offset / il.skew are left
out and refused by to-device), read-only state, aliases.

Exit: 0 ok, 1 a check failed or the device refused something, 2 usage.
"""
import argparse
import fnmatch
import json
import os
import re
import socket
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODEL_DIR = os.path.join(ROOT, 'firmware', 'src', 'model')
SRC_DIR = os.path.join(ROOT, 'firmware', 'src')
FORMAT = 'openscope-profile/1'

COLUMNS = ['sym', 'id', 'name', 'type', 'unit', 'decimals', 'group', 'inst', 'arg', 'min', 'max', 'step',
           'def', 'flags', 'labels', 'label_hook', 'get', 'set', 'store', 'owner']
# A label hook whose text is a fixed name table (so the names are canonical, as for a label array).
HOOK_TABLES = {'meas_label': 'prm_labels'}
STATUSES = ('OK', 'CLAMPED', 'INVALID', 'REFUSED', 'NOT_SET', 'BUSY', 'TIMEOUT')


class TableError(Exception):
    pass


# ═══ the table, from the firmware source ══════════════════════════════════════════════════════════════

def split_args(text):
    """Top-level comma split of a macro argument list, strings and parentheses respected."""
    out, cur, depth, i, n = [], [], 0, 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == '\\' else 1
            cur.append(text[i:j + 1])
            i = j + 1
            continue
        if c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
        if c == ',' and depth == 0:
            out.append(''.join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    out.append(''.join(cur).strip())
    return out


def c_string(tok):
    """The value of one or more adjacent C string literals."""
    parts = re.findall(r'"((?:\\.|[^"\\])*)"', tok)
    if not parts:
        raise TableError('not a string literal: %s' % tok)
    return ''.join(parts).replace('\\"', '"').replace('\\\\', '\\')


def strip_c_comments(text):
    return re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)


def find_string_array(name, sources):
    """The strings of `... name[...] = { "a", "b" };` in the firmware sources, or None."""
    pat = re.compile(r'\b' + re.escape(name) + r'\s*\[[^\]]*\]\s*=\s*\{(.*?)\}\s*;', re.S)
    for text in sources.values():
        m = pat.search(text)
        if m:
            return [c_string(t) for t in re.findall(r'"(?:\\.|[^"\\])*"', m.group(1))]
    return None


class Prop:
    def __init__(self, f):
        self.__dict__.update(f)

    def has(self, flag):
        return flag in self.flags

    @property
    def settable(self):
        return not self.has('PF_RO')

    @property
    def in_profile(self):
        """Saved in the user settings: what a profile is made of."""
        return self.has('PF_PERSIST') and self.settable and not self.has('PF_ALIAS')

    # ── value <-> text, as model.c's prop_format_value / prop_parse ──
    def canonical(self, v):
        if self.type == 'PT_BOOL':
            return 'on' if v else 'off'
        if self.type == 'PT_ENUM':
            if self.label_list and not self.measured and self.min <= v <= self.max:
                return self.label_list[v - self.min]
            return str(v)
        if self.type == 'PT_MASK':
            if v == 0:
                return 'none'
            names = []
            for b in range(31):
                if v & (1 << b):
                    names.append(self.label_list[b] if self.label_list and b < len(self.label_list)
                                 else 'bit%d' % b)
            return ','.join(names)
        return fmt_fixed(v, self.decimals)

    def text(self, v):
        """The profile text of v: the canonical value, with the unit for an int."""
        t = self.canonical(v)
        if self.type == 'PT_INT' and self.unit_symbol:
            t += ' ' + self.unit_symbol
        return t

    def parse(self, s):
        """Text -> value (absolute forms only, as a profile line). ValueError if it is not one."""
        s = s.split('#', 1)[0].strip()
        if not s:
            raise ValueError('empty value')
        low = s.lower()
        if self.type == 'PT_BOOL':
            if low in ('on', 'true', 'yes', '1'):
                return 1
            if low in ('off', 'false', 'no', '0'):
                return 0
        if self.type == 'PT_ENUM' and self.label_list and not self.measured:
            hits = [i for i, t in enumerate(self.label_list) if t.lower() == low]
            if len(hits) == 1:
                return self.min + hits[0]
        if self.type == 'PT_MASK':
            if low == 'none':
                return 0
            if low == 'all':
                return self.max
            if low.startswith('0x'):
                return int(low, 16)
            if low[0].isdigit():
                return int(low, 10)
            acc = 0
            for tok in s.split(','):
                hits = [i for i, t in enumerate(self.label_list or []) if t.lower() == tok.strip().lower()]
                if len(hits) != 1:
                    raise ValueError("no bit named '%s'" % tok.strip())
                acc |= 1 << hits[0]
            return acc
        t = s
        if self.type == 'PT_INT' and self.unit_symbol and len(t) > len(self.unit_symbol) and \
                t.lower().endswith(self.unit_symbol.lower()):
            t = t[:-len(self.unit_symbol)].strip()
        return parse_fixed(t, self.decimals if self.type == 'PT_INT' else 0)

    def in_range(self, v):
        if self.type == 'PT_BOOL':
            return v in (0, 1)
        if self.type == 'PT_MASK':
            return v >= 0 and (v & ~self.max) == 0
        return self.min <= v <= self.max

    def range_text(self):
        if self.type == 'PT_BOOL':
            return 'off/on'
        if self.type == 'PT_ENUM':
            if self.label_list and not self.measured:
                return '|'.join(self.label_list)
            return '%d..%d' % (self.min, self.max)
        if self.type == 'PT_MASK':
            return '0x%X' % self.max
        return '%s..%s%s step %s' % (fmt_fixed(self.min, self.decimals), fmt_fixed(self.max, self.decimals),
                                     ' ' + self.unit_symbol if self.unit_symbol else '',
                                     fmt_fixed(self.step, self.decimals))

    def saved_where(self):
        for flag, t in (('PF_RO', 'read only'), ('PF_ALIAS', 'alias'), ('PF_PERSIST', 'user settings'),
                        ('PF_USERCAL', 'usercal'), ('PF_SESSION', 'session')):
            if self.has(flag):
                return t
        return '?'


def fmt_fixed(v, decimals):
    mag, div = abs(v), 10 ** decimals
    s = ('-' if v < 0 else '') + str(mag // div)
    if decimals:
        s += '.' + str(mag % div).rjust(decimals, '0')
    return s


def parse_fixed(t, decimals):
    m = re.fullmatch(r'([+-]?)(\d+)(?:\.(\d*))?', t.strip())
    if not m:
        raise ValueError("not a number: '%s'" % t)
    frac = m.group(3) or ''
    if len(frac) > decimals and frac[decimals:].strip('0'):
        raise ValueError("'%s' has more than %d decimal(s)" % (t, decimals))
    v = int(m.group(2)) * 10 ** decimals + int((frac[:decimals]).ljust(decimals, '0') or '0')
    return -v if m.group(1) == '-' else v


def load_table(model_dir=MODEL_DIR, src_dir=SRC_DIR):
    """[Prop] in table order. Raises TableError on anything it cannot read exactly."""
    text = strip_c_comments(open(os.path.join(model_dir, 'model_table.h')).read())
    sources = {}
    for root, dirs, files in os.walk(src_dir):
        dirs[:] = sorted(d for d in dirs if d != 'attic')
        for f in sorted(files):
            if f.endswith('.c'):
                sources[os.path.join(root, f)] = strip_c_comments(open(os.path.join(root, f), errors='replace').read())
    model_h = strip_c_comments(open(os.path.join(model_dir, 'model.h')).read())
    m = re.search(r'typedef\s+enum\s*\{([^}]*)\}\s*prop_unit_t\s*;', model_h)
    if not m:
        raise TableError('prop_unit_t not found in model.h')
    unit_names = [u.strip() for u in m.group(1).split(',') if u.strip() and u.strip() != 'UNIT_COUNT']
    symbols = find_string_array('k_unit_symbol', sources)
    if symbols is None or len(symbols) != len(unit_names):
        raise TableError('k_unit_symbol does not match prop_unit_t (%s vs %d units)'
                         % (None if symbols is None else len(symbols), len(unit_names)))
    unit_symbol = dict(zip(unit_names, symbols))

    props, pos = [], 0
    while True:
        m = re.compile(r'\bPROP\s*\(').search(text, pos)
        if not m:
            break
        depth, i, in_str = 1, m.end(), False
        while depth and i < len(text):
            c = text[i]
            if in_str:
                if c == '\\':
                    i += 1
                elif c == '"':
                    in_str = False
            elif c == '"':
                in_str = True
            elif c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
            i += 1
        if depth:
            raise TableError('unterminated PROP( at offset %d' % m.start())
        args = split_args(text[m.end():i - 1])
        pos = i
        if len(args) != len(COLUMNS):
            raise TableError('PROP(%s...): %d columns, expected %d' % (args[0], len(args), len(COLUMNS)))
        f = dict(zip(COLUMNS, args))
        try:
            for k in ('id', 'decimals', 'arg', 'min', 'max', 'step', 'def'):
                f[k] = int(f[k], 0)
        except ValueError:
            raise TableError('%s: column %s is not a number (%s): a build profile needs literals'
                             % (f['name'], k, f[k]))
        f['name'] = c_string(f['name'])
        f['owner'] = c_string(f['owner'])
        f['flags'] = [x.strip() for x in f['flags'].split('|')]
        if f['unit'] not in unit_symbol:
            raise TableError('%s: unknown unit %s' % (f['name'], f['unit']))
        f['unit_symbol'] = unit_symbol[f['unit']]
        f['label_list'], f['measured'] = None, False
        if f['labels'] != 'NULL':
            f['label_list'] = find_string_array(f['labels'], sources)
            if f['label_list'] is None:
                raise TableError('%s: label array %s not found in the sources' % (f['name'], f['labels']))
        if f['label_hook'] != 'NULL':
            if f['label_hook'] in HOOK_TABLES:
                f['label_list'] = find_string_array(HOOK_TABLES[f['label_hook']], sources)
                if f['label_list'] is None:
                    raise TableError('%s: %s not found' % (f['name'], HOOK_TABLES[f['label_hook']]))
            else:
                f['measured'] = True          # the text moves with calibration: the index is canonical
        p = Prop(f)
        if p.type == 'PT_ENUM' and p.label_list and not p.measured:
            if len(p.label_list) < p.max - p.min + 1:
                raise TableError('%s: %d labels for %d values' % (p.name, len(p.label_list), p.max - p.min + 1))
            p.label_list = p.label_list[:p.max - p.min + 1]      # an array may name more than the range
        if not p.in_range(p.__dict__['def']):
            raise TableError('%s: default out of range' % p.name)
        props.append(p)
    if len(props) < 60:
        raise TableError('only %d properties parsed' % len(props))
    if len({p.name for p in props}) != len(props) or len({p.sym for p in props}) != len(props):
        raise TableError('duplicate name or symbol')
    return props


def default_of(p):
    return p.__dict__['def']


# ═══ profiles ══════════════════════════════════════════════════════════════════════════════════════

def profile_from_values(props, values, name, source, labels=None):
    """values: {prop name: int}. The profile dict, settings in table order."""
    prof = {'format': FORMAT, 'name': name, 'source': source, 'settings': {}}
    for p in props:
        if p.name in values:
            prof['settings'][p.name] = p.text(values[p.name])
    if labels:
        prof['labels'] = {k: v for k, v in labels.items() if k in prof['settings']}
    return prof


def defaults_profile(props):
    return profile_from_values(props, {p.name: default_of(p) for p in props if p.in_profile}, 'default',
                               'built-in defaults (firmware/src/model/model_table.h)')


def write_profile(prof, path):
    text = json.dumps(prof, indent=2, ensure_ascii=True) + '\n'
    if path in (None, '-'):
        sys.stdout.write(text)
    else:
        with open(path, 'w') as fh:
            fh.write(text)


def read_profile(path, props):
    """(profile, {name: int}, [problem]) - every setting checked against the table."""
    prof = json.load(open(path))
    problems = []
    if prof.get('format') != FORMAT or not isinstance(prof.get('settings'), dict):
        raise TableError('%s: not a profile (format %r)' % (path, prof.get('format')))
    by_name = {p.name: p for p in props}
    values = {}
    for name, text in prof['settings'].items():
        p = by_name.get(name)
        if p is None:
            problems.append('%s: unknown property' % name)
            continue
        if not p.settable or p.has('PF_ALIAS'):
            problems.append('%s: %s, not a setting' % (name, p.saved_where()))
            continue
        if p.has('PF_USERCAL'):
            problems.append('%s: user calibration, not carried by a profile' % name)
            continue
        try:
            v = p.parse(str(text))
        except ValueError as exc:
            problems.append('%s: bad value %r (%s)' % (name, text, exc))
            continue
        if not p.in_range(v):
            problems.append('%s: %r is out of range (%s)' % (name, text, p.range_text()))
            continue
        values[name] = v
    return prof, values, problems


# ═══ the device shell ══════════════════════════════════════════════════════════════════════════════

class ShellError(Exception):
    pass


class SocketPort:
    """socket://host:port with the two calls this tool needs (no pyserial)."""

    def __init__(self, url, timeout=0.05):
        host, _, port = url[len('socket://'):].split('/')[0].rpartition(':')
        self.s = socket.create_connection((host, int(port)), timeout=3.0)
        self.s.settimeout(timeout)

    def write(self, data):
        self.s.sendall(data)

    def read(self, n):
        try:
            return self.s.recv(n)
        except socket.timeout:
            return b''

    def close(self):
        self.s.close()


def open_port(url):
    if url.startswith('socket://'):
        try:
            import serial
        except ImportError:
            return SocketPort(url)
        return serial.serial_for_url(url, baudrate=115200, timeout=0.05)
    try:
        import serial
    except ImportError:
        raise ShellError('pyserial is needed for %s (socket://HOST:PORT works without it)' % url)
    return serial.serial_for_url(url, baudrate=115200, timeout=0.05)


class Shell:
    """One command, its reply lines (the echo and the `>` prompt removed)."""

    def __init__(self, port):
        self.port = port
        time.sleep(0.1)
        while self.port.read(4096):          # a banner, a stale tail
            pass

    def cmd(self, line, timeout=6.0):
        self.port.write((line + '\r\n').encode())
        buf, deadline = bytearray(), time.time() + timeout
        while time.time() < deadline:
            chunk = self.port.read(4096)
            if chunk:
                buf += chunk
                if buf.rstrip().endswith(b'>'):
                    text = buf.decode('utf-8', 'replace').rstrip()[:-1]
                    lines = [x.strip() for x in text.replace('\r', '').split('\n')]
                    return [x for x in lines if x and x != line]
            else:
                time.sleep(0.005)
        raise ShellError('no prompt within %.1f s after %r' % (timeout, line))

    def close(self):
        self.port.close()


def reply_status(first_line):
    """The status word a `set` answered with (its first line), INVALID for anything else."""
    status = first_line.split(' ', 1)[0].rstrip(':')
    return status if status in STATUSES else 'INVALID'


def parse_get_line(line, by_name):
    """'name=value[ unit][  # label]' -> (prop, int, label or None); None for another line."""
    if '=' not in line or line.startswith(('ERR', 'usage', '#')):
        return None
    name, _, rest = line.partition('=')
    p = by_name.get(name.strip())
    if p is None:
        return None
    value, _, label = rest.partition('#')
    return p, p.parse(value), (label.strip() or None)


def device_values(sh, props):
    """{name: int}, {name: label}: one `get *` (a single snapshot on the device)."""
    by_name = {p.name: p for p in props}
    values, labels = {}, {}
    for line in sh.cmd('get *'):
        try:
            got = parse_get_line(line, by_name)
        except ValueError as exc:
            raise ShellError('cannot read %r: %s (firmware and this checkout differ?)' % (line, exc))
        if got:
            p, v, lab = got
            values[p.name] = v
            if lab:
                labels[p.name] = lab
    missing = [p.name for p in props if p.name not in values]
    if missing:
        raise ShellError('the device did not report: %s (firmware and this checkout differ?)'
                         % ', '.join(missing[:8]))
    return values, labels


def cmd_from_device(props, a):
    sh = Shell(open_port(a.port))
    try:
        build = next((x for x in sh.cmd('version') if x.startswith('Build:')), 'Build: ?')
        values, labels = device_values(sh, props)
    finally:
        sh.close()
    keep = {p.name: values[p.name] for p in props
            if p.in_profile or (a.session and p.has('PF_SESSION') and p.settable and not p.has('PF_ALIAS'))}
    prof = profile_from_values(props, keep, a.name or 'device', 'device %s, %s' % (a.port, build), labels)
    write_profile(prof, a.output)
    changed = sum(1 for p in props if p.name in keep and keep[p.name] != default_of(p))
    print('%d settings read, %d differ from the built-in defaults%s'
          % (len(keep), changed, '' if a.output in (None, '-') else ' -> ' + a.output), file=sys.stderr)
    return 0


def cmd_to_device(props, a):
    prof, values, problems = read_profile(a.profile, props)
    for x in problems:
        print('SKIPPED  %s' % x)
    by_name = {p.name: p for p in props}
    order = [p for p in props if p.name in values]          # table order: the firmware's own apply order
    if a.dry_run:
        for p in order:
            print('would set %s %s' % (p.name, p.text(values[p.name])))
        print('%d settings, %d skipped (dry run: nothing sent)' % (len(order), len(problems)))
        return 1 if problems else 0
    sh = Shell(open_port(a.port))
    counts = dict.fromkeys(STATUSES + ('STORED', 'SAME'), 0)
    bad = len(problems)
    try:
        before, _ = device_values(sh, props)
        for p in order:
            want = values[p.name]
            if before[p.name] == want:
                counts['SAME'] += 1
                continue
            reply = sh.cmd('set %s %s' % (p.name, p.text(want)))
            first = reply[0] if reply else ''
            status = reply_status(first)
            counts[status] += 1
            if status == 'OK' and 'stored:' in first:
                counts['STORED'] += 1
                print('STORED   %s  (applied when its instrument is next entered)  %s' % (p.name, first))
            elif status != 'OK':
                if status != 'CLAMPED':
                    bad += 1
                print('%-8s %s  wanted %s  device: %s' % (status, p.name, p.text(want), ' / '.join(reply) or '(no reply)'))
        after, _ = device_values(sh, props)
    finally:
        sh.close()
    differ = [p for p in order if after[p.name] != values[p.name]]
    for p in differ:
        print('DIFFERS  %s  profile %s  device %s' % (p.name, p.text(values[p.name]), p.text(after[p.name])))
    del by_name
    print('%d settings: %d already so, %d set (%d stored for later), %d clamped, %d refused or failed, '
          '%d skipped; %d differ from the profile after the apply'
          % (len(order), counts['SAME'], counts['OK'], counts['STORED'], counts['CLAMPED'],
             bad - len(problems), len(problems), len(differ)))
    return 1 if (bad or differ) else 0


# ═══ list / diff / build ═══════════════════════════════════════════════════════════════════════════

def cmd_list(props, a):
    rows = [p for p in props if not a.glob or fnmatch.fnmatch(p.name, a.glob) or p.name.startswith(a.glob)]
    if not rows:
        print("no property matches '%s'" % a.glob)
        return 1
    print('%-17s %-6s %-5s %-38s %-14s %-13s %s' % ('name', 'id', 'type', 'range', 'default', 'saved', 'flags'))
    for p in rows:
        flags = ','.join(x[3:] for x in p.flags if x not in ('PF_PERSIST', 'PF_SESSION', 'PF_USERCAL'))
        rng = p.range_text()
        print('%-17s 0x%04X %-5s %-38s %-14s %-13s %s' % (p.name, p.id, p.type[3:].lower(),
              rng if len(rng) <= 38 else rng[:35] + '...', p.text(default_of(p)), p.saved_where(), flags))
    print('%d properties; %d saved in the user settings' % (len(rows), sum(1 for p in rows if p.in_profile)))
    return 0


def cmd_defaults(props, a):
    write_profile(defaults_profile(props), a.output)
    return 0


def cmd_diff(props, a):
    prof, values, problems = read_profile(a.profile, props)
    for x in problems:
        print('PROBLEM  %s' % x)
    if a.other:
        oprof, other, oproblems = read_profile(a.other, props)
        for x in oproblems:
            print('PROBLEM  %s (%s)' % (x, a.other))
        what = oprof.get('name', a.other)
    else:
        other = {p.name: default_of(p) for p in props}
        what = 'built-in default'
    n = 0
    for p in props:
        if p.name in values and p.name in other and values[p.name] != other[p.name]:
            n += 1
            print('%-17s %-24s %s %s' % (p.name, p.text(values[p.name]), what, p.text(other[p.name])))
    absent = [p.name for p in props if p.in_profile and p.name not in values]
    print("%s: %d settings, %d differ from the %s%s" % (prof.get('name', a.profile), len(values), n,
          what + ('s' if not a.other else ''), '; not in the profile: ' + ' '.join(absent) if absent else ''))
    return 1 if problems else 0


def build_header(props, prof, values, path_name):
    """The text of the generated header: PROFILE_DEF_<SYM> for every property (the profile's value where it
    has one, else the table's), and the profile's name. model.c takes `def` from these when it is built with
    the header."""
    lines = ['/* Generated by scripts/model_config.py build from %s. Do not edit. */' % path_name,
             '#ifndef MODEL_PROFILE_H', '#define MODEL_PROFILE_H',
             '#define MODEL_PROFILE_NAME "%s"' % re.sub(r'[^A-Za-z0-9_.-]', '_', str(prof.get('name', 'profile')))[:24],
             '#define MODEL_PROFILE_OVERRIDES %d' % sum(1 for p in props if p.name in values
                                                        and values[p.name] != default_of(p))]
    for p in props:
        v = values.get(p.name, default_of(p))
        lines.append('#define PROFILE_DEF_%s %s%s' % (p.sym, '(%d)' % v if v < 0 else str(v),
                     '   /* %s: %s (table: %s) */' % (p.name, p.text(v), p.text(default_of(p)))
                     if v != default_of(p) else ''))
    lines += ['#endif /* MODEL_PROFILE_H */', '']
    return '\n'.join(lines)


def cmd_build(props, a):
    prof, values, problems = read_profile(a.profile, props)
    by_name = {p.name: p for p in props}
    for name in list(values):
        if not by_name[name].has('PF_PERSIST'):
            # A session property's start value is its owner's initialiser, not the table's `def`: an
            # override would change what Reset gives and not what a boot gives.
            problems.append('%s: session only, its boot value is not taken from the table' % name)
            del values[name]
    if problems:
        for x in problems:
            print('REFUSED  %s' % x)
        print('build: profile refused (%d problems); nothing written' % len(problems))
        return 1
    text = build_header(props, prof, values, os.path.basename(a.profile))
    os.makedirs(os.path.dirname(os.path.abspath(a.output)), exist_ok=True)
    old = open(a.output).read() if os.path.exists(a.output) else None
    if old != text:                                   # unchanged: keep the timestamp, no rebuild
        with open(a.output, 'w') as fh:
            fh.write(text)
    n = sum(1 for p in props if p.name in values and values[p.name] != default_of(p))
    print('build: %s -> %s (%d of %d defaults overridden)' % (a.profile, a.output, n, len(props)))
    return 0


# ═══ self-test: a fake shell ═══════════════════════════════════════════════════════════════════════

class FakeDevice:
    """The shell's get / set / version over the same table: clamps ints, refuses what the firmware's rules
    refuse in the cases the tests use, answers in the firmware's words (model_shell.c)."""

    def __init__(self, props):
        self.props = props
        self.by_name = {p.name: p for p in props}
        self.v = {p.name: default_of(p) for p in props}
        self.deferred = set()          # names whose set is "stored" (their instrument's path is not live)
        self.log = []

    def line(self, p):
        v = self.v[p.name]
        t = p.text(v)
        if p.measured:
            t += '  # ~label%d' % v
        return '%s=%s' % (p.name, t)

    def handle(self, cmd):
        self.log.append(cmd)
        word, _, rest = cmd.strip().partition(' ')
        rest = rest.strip()
        if word == 'version':
            return ['OpenScope 2C53T', 'Build: fake shell, gcc 0']
        if word == 'get':
            out = []
            for pat in rest.split():
                hits = [p for p in self.props if fnmatch.fnmatchcase(p.name, pat)]
                if not hits:
                    out.append("ERR unknown property '%s' (try: list)" % pat)
                out += [self.line(p) for p in hits]
            return out
        if word == 'set':
            name, _, val = rest.partition(' ')
            p = self.by_name.get(name)
            if p is None:
                return ['INVALID %s (unknown property); nothing applied' % name]
            if not p.settable:
                return ['INVALID %s (read only); nothing applied' % name]
            try:
                v = p.parse(val)
            except ValueError:
                return ['INVALID %s=%s (bad value; %s); nothing applied' % (name, val, p.range_text())]
            status = 'OK'
            if not p.in_range(v):
                if p.type != 'PT_INT':
                    return ['INVALID %s=%s (out of range); nothing applied' % (name, val)]
                v, status = max(p.min, min(p.max, v)), 'CLAMPED'
            if name == 'trig.source' and v == 1 and not self.v['ch2.enabled']:
                return ['REFUSED trig.source (CH2 is off); old value kept: %s' % p.text(self.v[name])]
            if name == 'ch1.coupling' and getattr(self, 'relay_fails', False):
                return ['NOT_SET ch1.coupling (relay readback failed); old value kept: %s' % p.text(self.v[name])]
            self.v[name] = v
            return ['%s %s%s' % (status, self.line(p), ' ; stored: scope FE off' if name in self.deferred else '')]
        return ['Unknown command: %s' % cmd]


class FakePort:
    """A port in-process: what is written is a command line, what is read is the reply and the prompt."""

    def __init__(self, dev):
        self.dev, self.out, self.acc = dev, bytearray(b'OpenScope 2C53T\r\n> '), bytearray()

    def write(self, data):
        self.acc += data
        while b'\n' in self.acc:
            line, _, rest = bytes(self.acc).partition(b'\n')
            self.acc = bytearray(rest)
            cmd = line.decode().strip()
            reply = self.dev.handle(cmd)
            self.out += (cmd + '\r\n' + ''.join(x + '\r\n' for x in reply) + '> ').encode()   # echo, reply, prompt

    def read(self, n):
        chunk = bytes(self.out[:n])
        del self.out[:n]
        return chunk

    def close(self):
        pass


def serve_fake(dev):
    """The fake shell on 127.0.0.1, an ephemeral port (never a fixed one: a broker to a real unit may be
    listening on 7321). Returns (port, stop)."""
    srv = socket.socket()
    srv.bind(('127.0.0.1', 0))
    srv.listen(4)
    srv.settimeout(0.1)
    stop = threading.Event()

    def client(conn):
        port = FakePort(dev)
        conn.settimeout(0.05)
        try:
            while not stop.is_set():
                out = port.read(4096)
                if out:
                    conn.sendall(out)
                try:
                    data = conn.recv(4096)
                except socket.timeout:
                    continue
                if not data:
                    break
                port.write(data)
        except OSError:
            pass
        finally:
            conn.close()

    def loop():
        while not stop.is_set():
            try:
                conn, _ = srv.accept()
            except socket.timeout:
                continue
            threading.Thread(target=client, args=(conn,), daemon=True).start()
        srv.close()

    threading.Thread(target=loop, daemon=True).start()
    return srv.getsockname()[1], stop


def self_test(c_defaults, c_profile=None, c_shell=None):
    import contextlib
    import io
    import tempfile
    fails = []

    def check(ok, what):
        if not ok:
            fails.append(what)
            print('SELF-TEST FAIL  %s' % what)

    def run(argv):
        """(exit code, stdout) of this tool's own command line."""
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(io.StringIO()):
            rc = main(argv)
        return rc, buf.getvalue()

    # ── the splitter and the number text ──
    check(split_args('A, "x, (y)", f(1, 2), 0x10') == ['A', '"x, (y)"', 'f(1, 2)', '0x10'], 'split_args')
    check(c_string('"a \\"b\\" c"') == 'a "b" c', 'c_string')
    for v, d, t in ((15, 1, '1.5'), (-5, 1, '-0.5'), (1000000, 6, '1.000000'), (0, 0, '0'), (-100, 0, '-100')):
        check(fmt_fixed(v, d) == t and parse_fixed(t, d) == v, 'fixed %d/%d' % (v, d))
    for bad in ('1.55', 'x', '', '1e3', '--1'):
        try:
            parse_fixed(bad, 1)
            check(False, 'parse_fixed accepted %r' % bad)
        except ValueError:
            pass

    # ── the table ──
    props = load_table()
    by = {p.name: p for p in props}
    check(len(props) >= 60, 'table size')
    for name, text in (('ch1.vdiv', '3'), ('ch1.coupling', 'DC'), ('gen.amp', '3.0 V'), ('gen.freq', '1000 Hz'),
                       ('sys.startup', 'restore'), ('sys.brightness', '100 %'), ('ch1.meas', 'Freq,Peri,Duty+,Vp-p,Vrms'),
                       ('ch1.bwlimit', 'off'), ('meter.link', 'screen'), ('il.gain', '1.000000')):
        check(by[name].text(default_of(by[name])) == text, 'default text of %s: %s' % (name, by[name].text(default_of(by[name]))))
    # owner strings hold commas and parentheses: the splitter must not cut them
    check(by['meter.fuseview'].owner == 'fuse tester page (the Show softkey): detail, table of all ratings, '
          'scan (draw / no draw), types', 'owner of meter.fuseview: %r' % by['meter.fuseview'].owner)
    check(by['ch1.coupling'].owner == 'fpga_apply_coupling (relay readback)' and
          by['meter.link'].owner.endswith('With a USB host it always runs.'), 'owner strings')
    # every value of every property: text -> value -> text
    n_round = 0
    for p in props:
        vals = range(p.min, p.max + 1) if p.type in ('PT_BOOL', 'PT_ENUM') else \
            sorted({p.min, p.max, default_of(p), (p.min + p.max) // 2}) if p.type == 'PT_INT' else \
            [0, 1, p.max, default_of(p), 0x0A5 & p.max]
        for v in vals:
            n_round += 1
            try:
                back = p.parse(p.text(v))
            except ValueError as exc:
                back = exc
            check(back == v, '%s: %d -> %r -> %r' % (p.name, v, p.text(v), back))
    check(by['ch1.coupling'].parse('ac') == 1 and by['gen.amp'].parse('1.5V') == 15 and
          by['gen.amp'].parse('1.5') == 15 and by['ch1.meas'].parse('0x507') == 0x507 and
          by['ch1.meas'].parse('all') == 0xFFF and by['sys.autooff'].parse('15min') == 1, 'parse forms')
    for name, bad in (('ch1.coupling', 'XX'), ('ch1.meas', 'Freq,Nope'), ('gen.amp', '1.55 V')):
        try:
            by[name].parse(bad)
            check(False, '%s accepted %r' % (name, bad))
        except ValueError:
            pass
    # the C side's own default lines (tests/test_model.c --dump-defaults), when given
    if c_defaults:
        c_lines = [x.rstrip('\n') for x in open(c_defaults) if x.strip()]
        mine = ['%s = %s' % (p.name, p.text(default_of(p))) for p in props]
        check(len(c_lines) == len(mine), 'C defaults: %d lines, table %d' % (len(c_lines), len(mine)))
        for a_, b_ in zip(c_lines, mine):
            check(a_.split('  #')[0] == b_, 'C default line %r, this tool %r' % (a_, b_))
    # ... and of an image built with a defaults profile (make DEFAULTS=...): the profile's value where it
    # names one, the table's otherwise
    if c_profile:
        prof_path, dump = c_profile
        _prof, pvals, pproblems = read_profile(prof_path, props)
        check(not pproblems and len(pvals) > 0, 'fixture profile: %s' % pproblems)
        c_lines = [x.rstrip('\n').split('  #')[0] for x in open(dump) if x.strip()]
        want = ['%s = %s' % (p.name, p.text(pvals.get(p.name, default_of(p)))) for p in props]
        check(c_lines == want, 'defaults of a profile build: %s'
              % [(a_, b_) for a_, b_ in zip(c_lines, want) if a_ != b_][:4])
        check(sum(1 for p in props if pvals.get(p.name, default_of(p)) != default_of(p)) >= 5,
              'the fixture profile changes too little to prove anything')
    # the lines the firmware's own shell code prints (tests/test_model_exec.c --dump-shell): every `get`
    # line reads back as the value the model held, every `set` answer as its status
    if c_shell:
        n_get = n_set = 0
        for raw in open(c_shell):
            kind, _, rest = raw.rstrip('\n').partition('\t')
            want, _, line = rest.partition('\t')
            if kind == 'GET':
                n_get += 1
                try:
                    got = parse_get_line(line, by)
                except ValueError as exc:
                    got = exc
                check(isinstance(got, tuple) and got[1] == int(want), 'get line %r: read %r, the model had %s'
                      % (line, got[1] if isinstance(got, tuple) else got, want))
            elif kind == 'SET':
                n_set += 1
                check(reply_status(line) == want, 'set answer %r: read %s, expected %s' % (line, reply_status(line), want))
        check(n_get == len(props) and n_set >= 10, 'shell dump: %d get lines, %d set answers' % (n_get, n_set))
    # a broken table is refused, loudly
    with tempfile.TemporaryDirectory() as d:
        os.makedirs(os.path.join(d, 'model'))
        good = open(os.path.join(MODEL_DIR, 'model_table.h')).read()
        with open(os.path.join(d, 'model', 'model.h'), 'w') as fh:
            fh.write(open(os.path.join(MODEL_DIR, 'model.h')).read())
        for what, text in (('a label array that is nowhere', good.replace('coupling_labels', 'no_such_labels')),
                           ('a default that is not a literal', good.replace(' 10, PF_HW|PF_PERSIST,  ', ' TB_DEFAULT, PF_HW|PF_PERSIST,  ', 1)),
                           ('a missing column', good.replace('PT_BOOL, UNIT_NONE, 0, PG_CH1,', 'PT_BOOL, UNIT_NONE, PG_CH1,', 1))):
            check(text != good, 'self-test fixture: %s' % what)
            with open(os.path.join(d, 'model', 'model_table.h'), 'w') as fh:
                fh.write(text)
            try:
                load_table(os.path.join(d, 'model'), SRC_DIR)
                check(False, 'accepted %s' % what)
            except TableError:
                pass

    # ── profiles: defaults, the committed one, diff ──
    with tempfile.TemporaryDirectory() as d:
        dflt = os.path.join(d, 'default.json')
        rc, _ = run(['defaults', '-o', dflt])
        prof = json.load(open(dflt))
        check(rc == 0 and prof['format'] == FORMAT and len(prof['settings']) == sum(1 for p in props if p.in_profile),
              'defaults profile')
        check(prof['settings']['gen.amp'] == '3.0 V' and 'il.gain' not in prof['settings'] and
              'ch1.enabled' not in prof['settings'] and 'sys.mode' not in prof['settings'], 'what a profile carries')
        committed = os.path.join(ROOT, 'profiles', 'default.json')
        if os.path.exists(committed):
            check(json.load(open(committed)) == prof, 'profiles/default.json is not the table: run '
                  '`scripts/model_config.py defaults -o profiles/default.json`')
        rc, out = run(['diff', dflt])
        check(rc == 0 and ' 0 differ from the built-in defaults' in out, 'diff of the defaults: %s' % out)
        rc, out = run(['list', 'gen.*'])
        check(rc == 0 and 'gen.amp' in out and '0.0..3.0 V step 0.1' in out and '6 properties' in out, 'list: %s' % out)

        # ── a fake device, in-process ──
        dev = FakeDevice(props)
        for name, v in (('ch1.vdiv', 5), ('ch1.coupling', 1), ('gen.freq', 440), ('gen.amp', 15),
                        ('sys.theme', 2), ('ch1.meas', 0x003), ('ch2.enabled', 0), ('il.gain', 1002000)):
            dev.v[name] = v
        global open_port
        real_open = open_port
        open_port = lambda url: FakePort(dev)                       # noqa: E731
        try:
            got = os.path.join(d, 'got.json')
            rc, _ = run(['from-device', '--port', 'fake', '-o', got, '--name', 'bench'])
            g = json.load(open(got))
            check(rc == 0 and g['name'] == 'bench' and 'Build: fake shell' in g['source'], 'from-device header')
            check(g['settings']['ch1.vdiv'] == '5' and g['labels']['ch1.vdiv'] == '~label5' and
                  g['settings']['ch1.coupling'] == 'AC' and g['settings']['gen.freq'] == '440 Hz' and
                  g['settings']['gen.amp'] == '1.5 V' and g['settings']['sys.theme'] == 'contrast' and
                  g['settings']['ch1.meas'] == 'Freq,Peri', 'from-device values: %s' % g['settings'])
            check('ch2.enabled' not in g['settings'] and 'il.gain' not in g['settings'], 'session / usercal left out')
            rc, _ = run(['from-device', '--port', 'fake', '-o', got + '.s', '--session'])
            check(rc == 0 and json.load(open(got + '.s'))['settings'].get('ch2.enabled') == 'off', '--session')
            rc, out = run(['diff', got])
            check(rc == 0 and ' 6 differ from the built-in defaults' in out and
                  re.search(r'gen\.amp\s+1\.5 V\s+built-in default 3\.0 V', out) is not None, 'diff: %s' % out)
            rc, out = run(['diff', got, dflt])
            check(rc == 0 and ' 6 differ from the default' in out, 'diff against another profile: %s' % out)

            # to-device on a device at its defaults: sets exactly the six, in table order
            dev2 = FakeDevice(props)
            open_port = lambda url: FakePort(dev2)                  # noqa: E731
            rc, out = run(['to-device', got, '--port', 'fake'])
            sets = [x for x in dev2.log if x.startswith('set ')]
            check(rc == 0 and len(sets) == 6 and '6 set (0 stored for later), 0 clamped, 0 refused or failed' in out and
                  '0 differ from the profile' in out, 'to-device: %s' % out)
            check([x.split()[1] for x in sets] == ['ch1.vdiv', 'ch1.coupling', 'ch1.meas', 'gen.freq', 'gen.amp', 'sys.theme'],
                  'to-device order: %s' % sets)
            check(all(dev2.v[k] == dev.v[k] for k in ('ch1.vdiv', 'ch1.coupling', 'gen.freq', 'gen.amp', 'sys.theme', 'ch1.meas')),
                  'to-device state')
            rc, out = run(['to-device', got, '--port', 'fake'])
            check(rc == 0 and '6' not in out.split(':')[1].split(',')[1] and ' 0 set ' in out, 'to-device again: nothing to set: %s' % out)
            rc, out = run(['to-device', got, '--port', 'fake', '--dry-run'])
            check(rc == 0 and 'would set gen.amp 1.5 V' in out and 'nothing sent' in out, 'dry run')

            # refused, clamped, stored, an owner failure, a bad profile: each is reported, exit 1
            edit = json.load(open(got))
            edit['settings'].update({'trig.source': 'CH2', 'gen.freq': '60000 Hz', 'tb.code': '4',
                                     'ch1.coupling': 'DC'})
            bad = os.path.join(d, 'bad.json')
            json.dump(edit, open(bad, 'w'))
            rc, out = run(['to-device', bad, '--port', 'fake', '--dry-run'])
            check(rc == 1 and 'SKIPPED  gen.freq' in out and 'out of range' in out, 'a value out of range is refused on the host: %s' % out)
            edit['settings']['gen.freq'] = '50000 Hz'
            json.dump(edit, open(bad, 'w'))
            dev3 = FakeDevice(props)
            dev3.v['ch2.enabled'] = 0
            dev3.v['ch1.coupling'] = 1
            dev3.relay_fails = True
            dev3.deferred.add('tb.code')
            real_parse = by['gen.freq'].max
            by['gen.freq'].max = 99999            # the host table is wider than this device's: it clamps
            open_port = lambda url: FakePort(dev3)                  # noqa: E731
            edit['settings']['gen.freq'] = '60000 Hz'
            json.dump(edit, open(bad, 'w'))
            rc, out = run(['to-device', bad, '--port', 'fake'])
            by['gen.freq'].max = real_parse
            check(rc == 1, 'to-device with refusals must exit 1')
            check('REFUSED  trig.source' in out and 'CH2 is off' in out, 'refused reported: %s' % out)
            check('NOT_SET  ch1.coupling' in out and 'relay readback failed' in out, 'owner failure reported')
            check('STORED   tb.code' in out, 'stored reported')
            check('DIFFERS  trig.source' in out and 'DIFFERS  ch1.coupling' in out, 'read back differs')
            check('2 refused or failed' in out, 'summary: %s' % out.strip().split('\n')[-1])
            for junk in ({'no.such': '1'}, {'ui.focus': 'zoom'}, {'il.gain': '1.000000'}, {'ch1.coupling': 'XX'},
                         {'sys.mode': 'home'}):
                json.dump({'format': FORMAT, 'name': 'x', 'settings': junk}, open(bad, 'w'))
                rc, out = run(['to-device', bad, '--port', 'fake', '--dry-run'])
                check(rc == 1 and 'SKIPPED' in out and '0 settings' in out, 'junk %s: %s' % (junk, out))
            json.dump({'format': 'other', 'settings': {}}, open(bad, 'w'))
            rc, out = run(['diff', bad])
            check(rc == 2, 'a file that is no profile')

            # ── build: a header for make DEFAULTS=... ──
            hdr = os.path.join(d, 'model_profile.h')
            rc, out = run(['build', got, '-o', hdr])
            h = open(hdr).read()
            check(rc == 0 and '#define MODEL_PROFILE_NAME "bench"' in h and '#define PROFILE_DEF_GEN_AMP 15 ' in h and
                  '#define PROFILE_DEF_CH1_ENABLED 1\n' in h and '#define MODEL_PROFILE_OVERRIDES 6' in h and
                  h.count('#define PROFILE_DEF_') == len(props), 'build header: %s' % out)
            rc, out = run(['build', dflt, '-o', hdr])
            check(rc == 0 and '(0 of %d defaults overridden)' % len(props) in out and
                  '#define MODEL_PROFILE_OVERRIDES 0' in open(hdr).read(), 'build of the defaults')
            json.dump({'format': FORMAT, 'name': 'x', 'settings': {'ch2.enabled': 'off'}}, open(bad, 'w'))
            rc, out = run(['build', bad, '-o', hdr + '.x'])
            check(rc == 1 and 'session only' in out and not os.path.exists(hdr + '.x'), 'build refuses a session default')

            # ── the same fake shell over a socket (the socket:// path), on an ephemeral port ──
            open_port = real_open
            port, stop = serve_fake(dev)
            check(port != 7321, 'ephemeral port')
            try:
                rc, _ = run(['from-device', '--port', 'socket://127.0.0.1:%d' % port, '-o', got + '.sock', '--name', 'bench'])
                check(rc == 0 and json.load(open(got + '.sock'))['settings'] == g['settings'], 'from-device over socket://')
                rc, out = run(['to-device', dflt, '--port', 'socket://127.0.0.1:%d' % port])
                check(rc == 0 and '6 set' in out and dev.v['gen.amp'] == 30 and dev.v['ch1.vdiv'] == 3, 'to-device over socket://: %s' % out)
            finally:
                stop.set()
        finally:
            open_port = real_open
    print('model_config self-test: %d properties, %d value round trips, %d failures' % (len(props), n_round, len(fails)))
    return 1 if fails else 0


# ═══ main ══════════════════════════════════════════════════════════════════════════════════════════

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0], epilog='See the header of this file.')
    ap.add_argument('--self-test', action='store_true')
    ap.add_argument('--c-defaults', metavar='FILE', help='self-test: the firmware model\'s own default lines')
    ap.add_argument('--c-shell', metavar='FILE', help='self-test: get lines and set answers of the firmware shell code')
    ap.add_argument('--c-profile', nargs=2, metavar=('PROFILE', 'FILE'),
                    help='self-test: a profile and the default lines of a model built with it')
    sub = ap.add_subparsers(dest='command')
    s = sub.add_parser('list')
    s.add_argument('glob', nargs='?', default='')
    s = sub.add_parser('defaults')
    s.add_argument('-o', '--output')
    s = sub.add_parser('from-device')
    s.add_argument('--port', required=True)
    s.add_argument('-o', '--output')
    s.add_argument('--name')
    s.add_argument('--session', action='store_true', help='the session-only properties too')
    s = sub.add_parser('to-device')
    s.add_argument('profile')
    s.add_argument('--port', required=True)
    s.add_argument('--dry-run', action='store_true')
    s = sub.add_parser('diff')
    s.add_argument('profile')
    s.add_argument('other', nargs='?')
    s = sub.add_parser('build')
    s.add_argument('profile')
    s.add_argument('-o', '--output', required=True)
    try:
        a = ap.parse_args(argv)
    except SystemExit as exc:
        return exc.code if isinstance(exc.code, int) else 2
    if a.self_test:
        return self_test(a.c_defaults, a.c_profile, a.c_shell)
    if not a.command:
        ap.print_usage()
        return 2
    try:
        props = load_table()
        return {'list': cmd_list, 'defaults': cmd_defaults, 'from-device': cmd_from_device,
                'to-device': cmd_to_device, 'diff': cmd_diff, 'build': cmd_build}[a.command](props, a)
    except (TableError, ShellError, OSError, json.JSONDecodeError) as exc:
        print('model_config: %s' % exc, file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
