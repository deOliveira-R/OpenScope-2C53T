#!/usr/bin/env python3
"""check_state_owners.py - one writer per setting (OpenScope ARCH round, the state-owner ratchet).

Every user setting is a property (firmware/src/model/model_table.h) whose value lives in a variable the
firmware already had: a field of scope_state_t, a main.c global, an owner module's state. The property's
setter is the ONE place that may change it at run time, because the setter is what applies it through the
owner driver, bumps the change epoch and notes the settings record. A direct store anywhere else
(`ss->timebase_idx = x`, `meter_submode = 3`) changes the value behind the model's back: no hardware apply,
no repaint, no save, and the shell's `get` then reports a value nothing applied.

This script counts those stores outside the allowed writers and fails when a new one appears. It is the
sibling of check_reg_owners.py (registers) and works the same way: an owners file, a baseline that only
shrinks, a self-test.

What counts as a store (every assignment form: = |= &= ^= += -= *= /= %= <<= >>=, and ++/-- before or after):
  1. a field of scope_state_t, channel_state_t, trigger_state_t or cursor_state_t that backs a property,
     where the expression is known to be scope state:
       v->timebase_idx            v declared in that function as a pointer to the struct (a local or a parameter)
       scope_state_get()->roll    a function that returns such a pointer (found in the sources: chan(), ...)
       ...->ch1.vdiv_idx  ...->ch2.enabled  ...->trigger.mode  ...->cursor.mode   the member chains
     A field of the same name in another struct (a snapshot, a stream record) is not counted.
  2. a global a property wraps: current_mode, startup_mode, meter_submode, meter_layout, math_enabled,
     math_op, scope_view, persist_enabled, fuse_type, fuse_rating_idx, fuse_view, fuse_scan_threshold_mv
     (not a file's own static variable of the same name).
A definition with an initialiser at file scope (`volatile uint8_t meter_submode = 0;`) is the built-in
default, not a store. Reads are never counted. Comments and string literals are ignored.

NOT seen (stated, not hidden): a whole-struct copy (`*ss = saved`, memcpy into the state), a store through
a pointer to the field (`uint8_t *p = &ss->timebase_idx; *p = 3`), a store through a pointer the function
did not declare with its type (a macro argument, a void pointer cast), and state an owner module keeps behind
its own setter functions (sys_prefs, theme, meas_select, signal_gen, fpga.c's interleave, scope_mask):
calling such a setter from outside the model is the same mistake and this script does not count it.

Owners:      scripts/state_owners.txt   "<STATE>  <file>[:<function>] ..."   (STATE may be * for every state;
             file relative to firmware/src; `dir/` = any file in it)
Exemptions:  a line comment containing  STATE-OWNER-EXEMPT(<reason>)  (reviewed)
Baseline:    scripts/state_owners_baseline.txt  "<file>\t<function>\t<STATE>\t<count>" - known foreign stores.
             Fails on any store not covered by the baseline (new file/function/state, or a higher count).
             --update-baseline rewrites it; a stale entry (fixed) is reported and, with --strict, fails,
             so the baseline only ever shrinks.
--census:    every store, owned or not.
--top:       the files with the most foreign stores.
--self-test: runs the checker on injected snippets (each store form, an initialiser, a read, a comment, an
             owner, an exemption) and on clean controls; exit 1 if any is misjudged.
Exit: 0 ok, 1 new foreign stores (or, with --strict, stale baseline entries), 2 usage.
"""
import argparse
import collections
import os
import re
import sys
import tempfile

# Fields that back a property, by struct: field -> the property (the state's name in the reports).
SCOPE_FIELDS = {'timebase_idx': 'tb.code', 'roll_step': 'tb.rollstep', 'trig_x': 'trig.x',
                'soft_trigger': 'trig.soft', 'true_scale': 'view.truescale', 'prm_hidden': 'view.meashide',
                'split_prm_hidden': 'view.splithide',
                'running': 'view.run', 'roll': 'tb.roll'}
CHANNEL_FIELDS = {'enabled': 'chN.enabled', 'vdiv_idx': 'chN.vdiv', 'coupling': 'chN.coupling',
                  'probe': 'chN.probe', 'bw_limit': 'chN.bwlimit', 'position': 'chN.pos'}
TRIGGER_FIELDS = {'mode': 'trig.mode', 'edge': 'trig.edge', 'source': 'trig.source', 'level': 'trig.level'}
CURSOR_FIELDS = {'mode': 'view.cursor'}
BY_TYPE = {'scope_state_t': SCOPE_FIELDS, 'channel_state_t': CHANNEL_FIELDS,
           'trigger_state_t': TRIGGER_FIELDS, 'cursor_state_t': CURSOR_FIELDS}
# Globals.
GLOBALS = {
    'current_mode': 'sys.mode', 'startup_mode': 'sys.startup', 'meter_submode': 'meter.func',
    'meter_layout': 'meter.layout', 'math_enabled': 'view.math', 'math_op': 'view.math',
    'scope_view': 'view.mode', 'persist_enabled': 'view.persist', 'fuse_type': 'meter.fusetype',
    'fuse_rating_idx': 'meter.fuserating', 'fuse_view': 'meter.fuseview',
    'fuse_scan_threshold_mv': 'meter.fusethresh',
}
# Functions that return a pointer to scope state (collect() adds the ones the sources define).
GETTERS = {'scope_state_get': 'scope_state_t'}
GETTER_DEF = re.compile(r'^\s*(?:static\s+)?(?:inline\s+)?(scope_state_t|channel_state_t|trigger_state_t|cursor_state_t)'
                        r'\s*\*\s*(\w+)\s*\(')

ASSIGN = r'\s*(?:(?:<<|>>|[-+*/%|&^])?=(?!=)|\+\+|--)'
PREINC = r'(?:\+\+|--)\s*'
PTR_DECL = re.compile(r'\b(scope_state_t|channel_state_t|trigger_state_t|cursor_state_t)\s*\*\s*(?:const\s+)?(\w+)')
FUNC_RE = re.compile(r'^(?!\s*(?:if|for|while|switch|return|else|do)\b)[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)\s*\([^;]*$')
EXEMPT_RE = re.compile(r'STATE-OWNER-EXEMPT\(([^)]+)\)')


def strip_comments(lines):
    """(code, raw) per line: comments and string contents removed from code."""
    out, inc = [], False
    for raw in lines:
        s = raw
        if inc:
            if '*/' in s:
                s = s.split('*/', 1)[1]
                inc = False
            else:
                out.append(('', raw))
                continue
        s = re.sub(r'"(?:\\.|[^"\\])*"', '""', s)
        s = re.sub(r"'(?:\\.|[^'\\])'", "' '", s)
        s = re.sub(r'/\*.*?\*/', ' ', s)
        if '/*' in s:
            s = s.split('/*', 1)[0]
            inc = True
        s = s.split('//', 1)[0]
        out.append((s, raw))
    return out


def scan_text(text, rel, getters=None):
    """[(rel, function, state, line_no, raw, exempt_reason)] for every store in one file."""
    getters = dict(GETTERS if getters is None else getters)
    lines = strip_comments(text.split('\n'))
    for code, _ in lines:                       # this file's own helpers (static chan())
        m = GETTER_DEF.match(code)
        if m:
            getters[m.group(2)] = m.group(1)
    # A file's own static variable with a global's name is not the global.
    shadowed = set()
    depth = 0
    for code, _ in lines:
        if depth == 0:
            for name in GLOBALS:
                if re.match(r'\s*static\b[^;(]*\b' + name + r'\s*(?:=|;|\[)', code):
                    shadowed.add(name)
        depth = max(0, depth + code.count('{') - code.count('}'))

    hits = []
    func, depth = None, 0
    ptrs = {}                                   # variable -> pointed type, in the current function
    for no, (code, raw) in enumerate(lines, 1):
        is_pp = code.lstrip().startswith('#')
        if depth == 0:
            m = FUNC_RE.match(code)
            if m and not is_pp:
                func = m.group(1)
                ptrs = {}
            dm = re.match(r'\s*#\s*define\s+(\w+)', code)
            if dm:
                func = '#define ' + dm.group(1)
                ptrs = {}
        for m in PTR_DECL.finditer(code):
            ptrs[m.group(2)] = m.group(1)
        at_file_scope = depth == 0 and not is_pp and '{' not in code
        found = []

        def add(state, start):
            found.append((state, start))

        def field_stores(prefix, fields):
            for name, state in fields.items():
                for m in re.finditer(prefix + r'\s*' + name + r'\b' + ASSIGN, code):
                    add(state, m.start())
                for m in re.finditer(PREINC + prefix + r'\s*' + name + r'\b(?!\s*(?:\[|\(|->|\.))', code):
                    add(state, m.start())

        # 1. fields, where the expression is scope state
        for var, typ in ptrs.items():
            field_stores(r'(?<![\w\.>])' + re.escape(var) + r'\s*->', BY_TYPE[typ])
        for fn, typ in getters.items():
            field_stores(r'\b' + re.escape(fn) + r'\s*\([^()]*\)\s*->', BY_TYPE[typ])
        field_stores(r'(?:->|\.)\s*ch[12]\s*\.', CHANNEL_FIELDS)
        field_stores(r'(?:->|\.)\s*trigger\s*\.', TRIGGER_FIELDS)
        field_stores(r'(?:->|\.)\s*cursor\s*\.', CURSOR_FIELDS)
        # 2. globals (not a member of something else, not a definition with an initialiser)
        for name, state in GLOBALS.items():
            if name in shadowed:
                continue
            for m in re.finditer(r'(?<![\w\.>])' + name + r'\b' + ASSIGN, code):
                before = code[:m.start()]
                if re.search(r'[A-Za-z_]\w*[\s\*]+$', before) and (at_file_scope or re.search(
                        r'\b(?:const|volatile|static|extern|bool|float|char|int|unsigned|u?int\d+_t|\w+_t)'
                        r'[\s\*]+$', before)):
                    continue                    # a definition: the global's initialiser, or a local
                add(state, m.start())
            for m in re.finditer(PREINC + name + r'\b', code):
                add(state, m.start())

        ex = EXEMPT_RE.search(raw)
        seen = set()
        for state, start in found:
            if (state, start) in seen:
                continue
            seen.add((state, start))
            hits.append((rel, func or '(file scope)', state, no, raw.strip(), ex.group(1) if ex else None))
        depth = max(0, depth + code.count('{') - code.count('}'))
    return hits


def load_owners(path):
    owners = collections.defaultdict(list)       # state or '*' -> [(file_or_dir, function or None)]
    cur = None
    for line in open(path):
        line = line.split('#', 1)[0].rstrip()
        if not line.strip():
            continue
        parts = line.split()
        if not line[0].isspace():
            cur = parts[0]
            parts = parts[1:]
        for o in parts:
            f, _, fn = o.partition(':')
            owners[cur].append((f, fn or None))
    return owners


def owned(owners, state, rel, func):
    for key in (state, '*'):
        for f, fn in owners.get(key, []):
            if f.endswith('/'):
                if rel.startswith(f):
                    return True
            elif rel == f and (fn is None or fn == func):
                return True
    return False


def collect(src):
    files = []
    for root, dirs, names in os.walk(src):
        dirs[:] = sorted(d for d in dirs if d != 'attic')
        for f in sorted(names):
            if f.endswith(('.c', '.h')):
                files.append(os.path.join(root, f))
    texts = {p: open(p, errors='replace').read() for p in files}
    getters = dict(GETTERS)
    for p in files:                              # non-static helpers declared in a header
        if not p.endswith('.h'):
            continue
        for code, _ in strip_comments(texts[p].split('\n')):
            m = GETTER_DEF.match(code)
            if m:
                getters[m.group(2)] = m.group(1)
    hits = []
    for p in files:
        hits += scan_text(texts[p], os.path.relpath(p, src).replace(os.sep, '/'), getters)
    return hits


def foreign(hits, owners):
    viol = collections.Counter()
    for rel, func, state, _no, _raw, exempt in hits:
        if exempt or owned(owners, state, rel, func):
            continue
        viol[(rel, func, state)] += 1
    return viol


def self_test():
    owners = collections.defaultdict(list)
    owners['*'] = [('model/', None), ('ui/scope_state.c', None)]
    owners['sys.mode'] = [('tasks/input_handler.c', 'enter_mode')]
    cases = [
        # (name, file, text, expected {state: count} of FOREIGN stores)
        ('typed local', 'drivers/x.c',
         'void f(void) {\n    scope_state_t *ss = scope_state_get();\n    ss->timebase_idx = 3;\n}\n', {'tb.code': 1}),
        ('chain, compound', 'drivers/x.c', 'void f(void) {\n    s->ch1.vdiv_idx += 1;\n}\n', {'chN.vdiv': 1}),
        ('post-increment', 'drivers/x.c', 'void f(scope_state_t *ss) {\n    ss->roll_step++;\n}\n', {'tb.rollstep': 1}),
        ('pre-decrement', 'drivers/x.c', 'void f(scope_state_t *ss) {\n    --ss->roll_step;\n}\n', {'tb.rollstep': 1}),
        ('own helper', 'drivers/x.c',
         'static channel_state_t *chan(unsigned i)\n{\n    return 0;\n}\nvoid f(void) {\n    chan(1)->vdiv_idx = 2;\n}\n',
         {'chN.vdiv': 1}),
        ('typed in another function only', 'drivers/x.c',
         'void g(scope_state_t *c) {\n    (void)c;\n}\nvoid f(rec_t *c) {\n    c->timebase_idx = 3;\n}\n', {}),
        ('same field name, another struct', 'stream/x.c',
         'void f(rec_t *c) {\n    c->vdiv_idx = 1;\n    R.snap.trig_x = ss->trig_x;\n    c->coupling[0] = 1;\n}\n', {}),
        ('file static of the same name', 'ui/x.c',
         'static int current_mode = 0;\nvoid f(void) {\n    current_mode = 2;\n}\n', {}),
        ('chN chain', 'drivers/x.c', 'void f(void) {\n    ss->ch2.enabled = false;\n}\n', {'chN.enabled': 1}),
        ('typed pointer', 'drivers/x.c',
         'void f(void) {\n    channel_state_t *c = &ss->ch1;\n    c->coupling = COUPLING_AC;\n}\n', {'chN.coupling': 1}),
        ('typed parameter', 'drivers/x.c', 'void f(scope_state_t *st) {\n    st->running = false;\n}\n', {'view.run': 1}),
        ('scope_state_get()', 'drivers/x.c', 'void f(void) {\n    scope_state_get()->roll = true;\n}\n', {'tb.roll': 1}),
        ('trigger chain', 'drivers/x.c',
         'void f(void) {\n    ss->trigger.mode = TRIG_AUTO; ss->trigger.level = 0;\n}\n',
         {'trig.mode': 1, 'trig.level': 1}),
        ('cursor chain', 'drivers/x.c', 'void f(void) {\n    ss->cursor.mode = CURSOR_OFF;\n}\n', {'view.cursor': 1}),
        ('global', 'drivers/x.c', 'void f(void) {\n    meter_submode = 3;\n    current_mode = MODE_HOME;\n}\n',
         {'meter.func': 1, 'sys.mode': 1}),
        ('global in a macro', 'drivers/x.h', '#define GO() (math_enabled = true)\n', {'view.math': 1}),
        ('two on a line', 'drivers/x.c', 'void f(void) {\n    math_enabled = true; math_op = 2;\n}\n', {'view.math': 2}),
        # controls: nothing foreign
        ('read', 'drivers/x.c',
         'int f(scope_state_t *ss) {\n    return ss->timebase_idx == 3 && meter_submode != 2;\n}\n', {}),
        ('comparison chain', 'drivers/x.c', 'int f(void) {\n    if (ss->ch1.enabled == true) return 1;\n    return 0;\n}\n', {}),
        ('initialiser at file scope', 'main.c', 'volatile uint8_t meter_submode = 0;\nvolatile bool math_enabled = false;\n', {}),
        ('comment', 'drivers/x.c',
         'void f(void) {\n    /* scope_state_get()->roll = 1; */\n    // meter_submode = 1;\n}\n', {}),
        ('string', 'drivers/x.c', 'void f(void) {\n    puts("set meter_submode = 3");\n}\n', {}),
        ('another struct, common name', 'ui/x.c', 'void f(void) {\n    f->probe = "X10";\n    w->enabled = true;\n}\n', {}),
        ('longer name', 'drivers/x.c', 'void f(void) {\n    my_meter_submode = 3;\n    cfg.meter_submode = 3;\n}\n', {}),
        ('local of the same name', 'drivers/x.c', 'void f(void) {\n    uint8_t math_op = 2;\n    (void)math_op;\n}\n', {}),
        ('owner dir', 'model/props_scope.c', 'void f(void) {\n    scope_state_get()->timebase_idx = 3;\n}\n', {}),
        ('owner file', 'ui/scope_state.c', 'void f(void) {\n    s->ch1.vdiv_idx = 8;\n}\n', {}),
        ('owner function', 'tasks/input_handler.c', 'static void enter_mode(int m)\n{\n    current_mode = m;\n}\n', {}),
        ('not the owner function', 'tasks/input_handler.c', 'static void other(int m)\n{\n    current_mode = m;\n}\n',
         {'sys.mode': 1}),
        ('exemption', 'drivers/x.c',
         'void f(void) {\n    scope_state_get()->timebase_idx = 3;   /* STATE-OWNER-EXEMPT(boot) */\n}\n', {}),
    ]
    bad = 0
    for name, rel, text, want in cases:
        got = collections.Counter()
        for (r, _fn, state), n in foreign(scan_text(text, rel), owners).items():
            got[state] += n
        if dict(got) != want:
            bad += 1
            print('SELF-TEST FAIL  %-34s want %s got %s' % (name, want, dict(got)))
    # the function a store is filed under
    hits = scan_text('static int g;\nvoid first(void)\n{\n    meter_layout = 1;\n}\nvoid second(void)\n{\n    meter_layout = 2;\n}\n',
                     'drivers/x.c')
    if [h[1] for h in hits] != ['first', 'second']:
        bad += 1
        print('SELF-TEST FAIL  function names: %s' % [h[1] for h in hits])
    # the baseline mechanism: a new store fails, a covered one passes, a fixed one is reported
    with tempfile.TemporaryDirectory() as d:
        os.makedirs(os.path.join(d, 'src', 'drivers'))
        with open(os.path.join(d, 'src', 'drivers', 'x.c'), 'w') as fh:
            fh.write('void f(void) {\n    meter_submode = 3;\n}\n')
        own = os.path.join(d, 'owners.txt')
        with open(own, 'w') as fh:
            fh.write('*  model/\n')
        base = os.path.join(d, 'base.txt')
        args = ['--src', os.path.join(d, 'src'), '--owners', own, '--baseline', base, '--quiet']
        if run(args) != 1:
            bad += 1
            print('SELF-TEST FAIL  a new store did not fail')
        if run(args + ['--update-baseline']) != 0 or run(args) != 0:
            bad += 1
            print('SELF-TEST FAIL  a baselined store did not pass')
        with open(os.path.join(d, 'src', 'drivers', 'x.c'), 'w') as fh:
            fh.write('void f(void) {\n    meter_submode = 3;\n    meter_submode = 4;\n}\n')
        if run(args) != 1:
            bad += 1
            print('SELF-TEST FAIL  a higher count did not fail')
        with open(os.path.join(d, 'src', 'drivers', 'x.c'), 'w') as fh:
            fh.write('void f(void) {\n}\n')
        if run(args) != 0 or run(args + ['--strict']) != 1:
            bad += 1
            print('SELF-TEST FAIL  a fixed store: plain must pass, --strict must fail')
    print('state owners self-test: %d cases, %d failed' % (len(cases) + 5, bad))
    return 1 if bad else 0


def run(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--src', default='firmware/src')
    ap.add_argument('--owners', default='scripts/state_owners.txt')
    ap.add_argument('--baseline', default='scripts/state_owners_baseline.txt')
    ap.add_argument('--update-baseline', action='store_true')
    ap.add_argument('--strict', action='store_true', help='a stale baseline entry fails too')
    ap.add_argument('--census', action='store_true', help='print every store, owned or not')
    ap.add_argument('--top', type=int, default=0, metavar='N', help='the N files with the most foreign stores')
    ap.add_argument('--self-test', action='store_true', help='check the checker on injected snippets')
    ap.add_argument('--quiet', action='store_true')
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    if not os.path.isdir(a.src) or not os.path.exists(a.owners):
        print('usage: run from the repository root (no %s or %s)' % (a.src, a.owners))
        return 2
    say = (lambda *x: None) if a.quiet else print
    owners = load_owners(a.owners)
    hits = collect(a.src)
    if a.census:
        for rel, func, state, no, raw, exempt in hits:
            tag = 'exempt' if exempt else ('owner ' if owned(owners, state, rel, func) else 'FOREIGN')
            say('%-7s %-16s %s:%d %s: %s' % (tag, state, rel, no, func, raw[:90]))
    viol = foreign(hits, owners)
    if a.update_baseline:
        with open(a.baseline, 'w') as fh:
            for (rel, func, state), n in sorted(viol.items()):
                fh.write('%s\t%s\t%s\t%d\n' % (rel, func, state, n))
        say('baseline: %d entries, %d stores' % (len(viol), sum(viol.values())))
        return 0
    base = {}
    if os.path.exists(a.baseline):
        for line in open(a.baseline):
            p = line.rstrip('\n').split('\t')
            if len(p) == 4:
                base[(p[0], p[1], p[2])] = int(p[3])
    new = {k: v for k, v in viol.items() if v > base.get(k, 0)}
    fixed = {k: v for k, v in base.items() if viol.get(k, 0) < v}
    for (rel, func, state), v in sorted(new.items()):
        say('NEW   %-30s %-34s %-16s x%d (baseline %d)' % (rel, func, state, v, base.get((rel, func, state), 0)))
    for (rel, func, state), v in sorted(fixed.items()):
        say('FIXED %-30s %-34s %-16s baseline %d now %d: run --update-baseline'
            % (rel, func, state, v, viol.get((rel, func, state), 0)))
    if a.top:
        per = collections.Counter()
        for (rel, _f, _s), n in viol.items():
            per[rel] += n
        for rel, n in per.most_common(a.top):
            states = collections.Counter()
            for (r, _f, s), k in viol.items():
                if r == rel:
                    states[s] += k
            say('  %3d  %-32s %s' % (n, rel, ', '.join('%s %d' % kv for kv in states.most_common())))
    covered = sum(min(v, base.get(k, 0)) for k, v in viol.items())
    say('state owners: %d stores scanned, %d outside the model (%d baselined), %d new, %d fixed'
        % (len(hits), sum(viol.values()), covered, sum(v - base.get(k, 0) for k, v in new.items()), len(fixed)))
    if new:
        say('A setting is changed through its property: prop_set_local() on the input task, prop_request() / '
            'prop_post() from another task.')
        return 1
    return 1 if (a.strict and fixed) else 0


if __name__ == '__main__':
    sys.exit(run(sys.argv[1:]))
