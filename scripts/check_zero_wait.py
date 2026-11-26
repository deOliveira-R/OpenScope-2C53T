#!/usr/bin/env python3
"""check_zero_wait.py - the hot code is in the zero-wait flash (OpenScope, flash census 2026-11-25).

With EOPB0=0xFE the AT32F403A reads only its first 128 KB of flash with no wait states; past it every
fetch waits. Measured: canvas_runs.o linked past that line made a live Y-T frame 37 ms instead of 23.6 ms.
ld/zero_wait_groups.ld puts the hot code first, in three ordered groups (per pixel; per record and ISR;
per frame), and at32f403a_app.ld ASSERTs each group's end. That catches a group that grew too big, not a
hot function the list does not name (a new helper in a new file links wherever the link order puts it).
This script reads the built ELF and its map and fails when:
  1. a group's end (_ezw_pixel, _ezw_record, _ezw_frame) is past the boundary (_flash_zw_end, read from
     the ELF, not typed again here);
  2. a function reachable from the roots below ends past the boundary, or is not inside the groups (it
     is fast only by link order, until something grows before it). The call graph is the disassembly's
     direct branches, the function addresses in literal pools, and the const function-pointer tables named
     in FUNCTION_TABLES (an edge from each function that loads the table to every function in it). The walk
     does not enter the functions in STOP (cold paths reachable from hot code: fatal errors, creation,
     enumeration, calibration), each with its reason;
  3. read-only data a reached function loads (and the data that data points to) is not inside the
     groups, unless DATA_LEFT_OUT names it with its reason;
  4. a name in the group list, STOP or DATA_LEFT_OUT no longer exists (a renamed function would leave the
     list silently placing nothing), or a listed object is not inside the groups.
Each failure names the function, its object, its class and the chain from its root, which is what the
list needs to take it. A function's class is the hottest root class that reaches it.
It prints the groups' ends and the margin left to the boundary.
Exit: 0 ok, 1 a check failed, 2 usage.
"""
import argparse, bisect, collections, os, re, struct, subprocess, sys

# The roots of each class (census section 2), hottest first.
ROOTS = collections.OrderedDict([
    ('per pixel', (                       # every pixel or column of a frame
        'yt_paint_column', 'yt_paint_base', 'fft_paint_column', 'xy_paint', 'waterfall_blit',
        'cursor_layer_paint', 'canvas_draw_columns')),
    ('per record and ISR', (              # every record (acquisition task) and every interrupt
        'fpga_acq_task', 'EXINT0_IRQHandler', 'EXINT3_IRQHandler', 'USART2_IRQHandler',
        'USBFS_L_CAN1_RX0_IRQHandler', 'TMR3_GLOBAL_IRQHandler', 'SysTick_Handler', 'PendSV_Handler',
        'SVC_Handler', 'vTaskSwitchContext')),
    ('per frame', (                       # every display frame (20 a second) and the strip's refresh
        'draw_scope_live_frame', 'draw_scope_screen', 'draw_xy_screen', 'draw_fft_screen',
        'draw_waterfall_screen', 'draw_split_screen', 'scope_ui_strip_refresh',
        'scope_ui_cursor_row_refresh', 'draw_status_bar', 'scope_view_tick', 'scope_render')),
])
GROUP_START = '_ezw_start'
GROUP_END = {'per pixel': '_ezw_pixel', 'per record and ISR': '_ezw_record', 'per frame': '_ezw_frame'}
BOUNDARY = '_flash_zw_end'

# Const tables of function pointers the frame dispatches through.
FUNCTION_TABLES = ('yt_overlays', 'yt_base', 'k_renderers', 'k_half_draw')

# Reachable from hot code, but not hot: the walk stops here.
STOP = {
    'fault_assert':                  'configASSERT and fatal checks: runs once, then the unit restarts',
    'fault_crash':                   'the crash path: runs once, then the unit restarts',
    'vApplicationMallocFailedHook':  'fatal: the heap is exhausted',
    'vApplicationStackOverflowHook': 'fatal: a task overran its stack',
    'pvPortMalloc':                  'allocation: task and queue creation, the X-Y snapshot buffers once',
    '_malloc_r':                     "nano-printf grows a buffer only for a growable string, never sniprintf's",
    '_realloc_r':                    "nano-printf grows a buffer only for a growable string, never sniprintf's",
    '_free_r':                       "nano-printf grows a buffer only for a growable string, never sniprintf's",
    'xQueueCreateMutex':             'creation, once per object',
    'xQueueGenericCreate':           'creation, once per object',
    'usbd_core_setup_handler':       'USB enumeration and control requests, at plug-in',
    'usbd_reset_handler':            'USB bus reset, at plug-in',
    'dfu_jump_to_bootloader':        'leaves the application',
    'dfu_restart_app':               'leaves the application',
    'ilcal_window':                  'interleave calibration capture, only while it runs',
    'mask_pf_teach_begin':           'mask teaching starts, per user action',
    'can_bus_analyse':               'CAN decoding: only with the CAN screen on, at its own rate',
    'can_scope_screen_line':         'CAN decoding: only with the CAN screen on, at its own rate',
    '__kernel_rem_pio2f':            'cosf/sinf past 2^7*pi/2 only; the hot callers (scope_freq, the FFT window) stay within 2*pi',
}

# Hot read-only data deliberately left in the slow flash.
DATA_LEFT_OUT = {
    'prop_table':   'the property table (4.7 KB): each prop_get call reads one entry',
    'font_large':   'the popup title font (4.4 KB): only while a popup is up',
    'stock_tables': "the generator's wave tables (1.6 KB): read only to draw the status bar's small wave icon",
}

FLASH = (0x08000000, 0x08100000)    # the AT32F403A's internal flash, 1 MB

# ELF32 (the System V ABI): section and symbol types, and the size of a symbol table entry.
SHT_PROGBITS, SHT_SYMTAB = 1, 2
STT_NOTYPE, STT_OBJECT, STT_FUNC = 0, 1, 2
SHN_ABS = 0xFFF1
SYM_SIZE = 16


def run(*cmd):
    return subprocess.run(cmd, capture_output=True, text=True, check=True).stdout


class Image:
    """The ELF's functions and read-only data, keyed by address (a static name can repeat across files)."""

    def __init__(self, elf, mapfile, objdump):
        self.elf = elf
        self._elf()
        self._graph(objdump)
        self._map(mapfile)

    def _elf(self):
        """The loaded sections and the symbol table. A symbol's ELF type says function or data (nm's
        letter does not: data placed in .text reads as code)."""
        raw = open(self.elf, 'rb').read()
        shoff, = struct.unpack_from('<I', raw, 0x20)
        shentsize, shnum = struct.unpack_from('<HH', raw, 0x2E)
        sh = [struct.unpack_from('<IIIIIIIIII', raw, shoff + i * shentsize) for i in range(shnum)]
        self.raw = raw
        self.secs = [(s[3], s[4], s[5]) for s in sh if s[1] == SHT_PROGBITS and s[3]]
        rows = []
        for s in sh:
            if s[1] != SHT_SYMTAB:
                continue
            strtab = sh[s[6]]
            for off in range(s[4], s[4] + s[5], SYM_SIZE):
                name, value, size, info, _, shndx = struct.unpack_from('<IIIBBH', raw, off)
                end = raw.index(b'\0', strtab[4] + name)
                rows.append((value, size, info & 0xF, shndx, raw[strtab[4] + name:end].decode()))
        self.addr = collections.defaultdict(list)  # name -> addresses (Thumb bit off)
        self.func = {}                     # address -> (name, size)
        self.data = {}                     # address -> (name, size), read-only data in flash
        self.marks = {}                    # linker symbols: name -> address
        placed = sorted({r[0] & ~1 for r in rows if r[2] in (STT_FUNC, STT_OBJECT)})
        for value, size, typ, shndx, name in rows:
            if typ == STT_NOTYPE and name.startswith('_') and (shndx == SHN_ABS or not size):
                self.marks[name] = value   # _flash_zw_end, _ezw_*, _etext
                continue
            a = value & ~1 if typ == STT_FUNC else value
            if not FLASH[0] <= a < FLASH[1]:
                continue
            if typ == STT_FUNC:
                if not size:               # an assembly function without .size: up to the next symbol
                    i = bisect.bisect_right(placed, a)
                    size = (placed[i] - a) if i < len(placed) else 0
                if size:
                    self.func.setdefault(a, (name, size))
                    self.addr[name].append(a)
            elif typ == STT_OBJECT and size:
                self.data.setdefault(a, (name, size))
                self.addr[name].append(a)
        self.data_at = sorted(self.data)

    def read(self, addr, n):
        for a, o, s in self.secs:
            if a <= addr and addr + n <= a + s:
                return self.raw[o + addr - a:o + addr - a + n]
        return b''

    def data_containing(self, v):
        i = bisect.bisect_right(self.data_at, v) - 1
        if i >= 0 and self.data_at[i] <= v < self.data_at[i] + self.data[self.data_at[i]][1]:
            return self.data_at[i]
        return None

    def words(self, addr):
        s = self.data[addr][1]
        b = self.read(addr, s - s % 4)
        return [struct.unpack_from('<I', b, i)[0] for i in range(0, len(b), 4)]

    def _graph(self, objdump):
        self.calls = collections.defaultdict(set)   # function -> functions it calls or takes
        self.loads = collections.defaultdict(set)   # function -> data it loads
        head = re.compile(r'^([0-9a-f]+) <.+>:$')
        word = re.compile(r'\.word\s+0x([0-9a-f]+)')
        branch = re.compile(r'^(?:b|bl|blx|b\.[wn]|b[a-z]{2}(?:\.[wn])?|cbn?z)\s+(?:\w+,\s*)?([0-9a-f]+) <[^>+]+>')
        cur = None
        for line in run(objdump, '-d', '--no-show-raw-insn', self.elf).splitlines():
            m = head.match(line)
            if m:
                a = int(m.group(1), 16)
                cur = a if a in self.func else None
                continue
            if cur is None or ':' not in line:
                continue
            ins = line.split(':', 1)[1].strip()
            m = word.match(ins)
            if m:
                v = int(m.group(1), 16)
                if v & 1 and (v & ~1) in self.func:
                    self.calls[cur].add(v & ~1)
                else:
                    d = self.data_containing(v)
                    if d is not None:
                        self.loads[cur].add(d)
                continue
            m = branch.match(ins)
            if m:
                t = int(m.group(1), 16)
                if t != cur and t in self.func:
                    self.calls[cur].add(t)
        for t in FUNCTION_TABLES:
            for ta in self.addr.get(t, ()):
                targets = {w & ~1 for w in self.words(ta) if w & 1 and (w & ~1) in self.func}
                for f, ds in self.loads.items():
                    if ta in ds:
                        self.calls[f] |= targets

    def _map(self, mapfile):
        """Input sections of the map: (start, size, object) for .text* and .rodata*."""
        self.inputs = []
        pend = None
        sec_only = re.compile(r'^ (\.(?:text|rodata)\S*)\s*$')
        full = re.compile(r'^ (\.(?:text|rodata)\S*)?\s+0x([0-9a-f]{8,16})\s+0x([0-9a-f]+)\s+(\S+)\s*$')
        for line in open(mapfile):
            line = line.rstrip('\n')
            m = sec_only.match(line)
            if m:
                pend = m.group(1)
                continue
            m = full.match(line)
            if m and (m.group(1) or pend):
                obj = m.group(4)
                am = re.match(r'.*\((.+\.o)\)$', obj)
                self.inputs.append((int(m.group(2), 16), int(m.group(3), 16),
                                    am.group(1) if am else os.path.basename(obj)))
            pend = None
        self.inputs.sort()
        self.input_at = [i[0] for i in self.inputs]

    def object_of(self, addr):
        i = bisect.bisect_right(self.input_at, addr) - 1
        if i >= 0 and self.inputs[i][0] <= addr < self.inputs[i][0] + self.inputs[i][1]:
            return self.inputs[i][2]
        return '?'

    def name(self, a):
        return (self.func.get(a) or self.data.get(a))[0]

    def size(self, a):
        return (self.func.get(a) or self.data.get(a))[1]

    def describe(self, a):
        return '%s (%s)' % (self.name(a), self.object_of(a))


def addresses(img, names):
    return {a for n in names for a in img.addr.get(n, ())}


def reach(img):
    """Class and parent of every reached function, hottest class first."""
    stop = addresses(img, STOP)
    cls, parent = {}, {}
    for c, roots in ROOTS.items():
        todo = sorted(addresses(img, roots))
        while todo:
            f = todo.pop()
            if f in cls or f in stop:
                continue
            cls[f] = c
            for g in sorted(img.calls.get(f, ())):
                if g not in cls and g not in stop:
                    parent.setdefault(g, f)
                    todo.append(g)
    return cls, parent


def hot_data(img, cls):
    """Read-only data loaded by reached functions, and the data whose start it holds (a pointer)."""
    left = addresses(img, DATA_LEFT_OUT)
    hot = {}
    todo = [(d, cls[f]) for f in cls for d in img.loads.get(f, ())]
    while todo:
        d, c = todo.pop()
        if d in hot or d in left:
            continue
        hot[d] = c
        for w in img.words(d):
            if w in img.data and w != d:
                todo.append((w, c))
    return hot


def chain(img, parent, f):
    out = [f]
    while parent.get(out[-1]) is not None:
        out.append(parent[out[-1]])
    return ' <- '.join(img.name(a) for a in out)


def listed(groups_file):
    """Objects, functions and data the group list names."""
    objs, funcs, data = set(), set(), set()
    entry = re.compile(r'\*(?:/|[\w.]+\.a:)([\w.+-]+\.o)\(([^)]*)\)')
    for line in open(groups_file):
        line = re.sub(r'/\*.*?\*/', '', line)
        for m in entry.finditer(line):
            secs = m.group(2).split()
            if any(s in ('.text', '.text*') for s in secs):
                objs.add(m.group(1))
            for s in secs:
                if s.startswith('.text.') and not s.endswith('.*'):
                    funcs.add(s[len('.text.'):])
                elif s.startswith('.rodata.') and not s.endswith('.*'):
                    data.add(s[len('.rodata.'):])
    return objs, funcs, data


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--elf', required=True)
    ap.add_argument('--map', required=True)
    ap.add_argument('--groups', required=True, help='the group list, ld/zero_wait_groups.ld')
    ap.add_argument('--objdump', default='arm-none-eabi-objdump')
    a = ap.parse_args()
    img = Image(a.elf, a.map, a.objdump)
    for need in (BOUNDARY, GROUP_START) + tuple(GROUP_END.values()):
        if need not in img.marks:
            print('check_zero_wait: %s is not in %s (linked with ld/at32f403a_app.ld?)' % (need, a.elf))
            return 2
    zw = img.marks[BOUNDARY]
    fails = []

    # 1. the groups' ends
    print('zero-wait boundary 0x%08X; the groups start at 0x%08X' % (zw, img.marks[GROUP_START]))
    prev = img.marks[GROUP_START]
    for c, mark in GROUP_END.items():
        e = img.marks[mark]
        print('  %-19s ends 0x%08X (%s), %6d bytes' % (c, e, mark, e - prev))
        if e > zw:
            fails.append('group "%s" ends 0x%08X, past the boundary' % (c, e))
        prev = e
    print('  margin after the groups: %d bytes' % (zw - prev))

    # 2. reached functions
    for c, roots in ROOTS.items():
        for r in roots:
            if r not in img.addr:
                fails.append('root %s (%s) is not in the image' % (r, c))
    cls, parent = reach(img)
    start = img.marks[GROUP_START]
    by = collections.Counter()
    for f, c in sorted(cls.items()):
        by[c] += img.size(f)
        if f + img.size(f) > zw:
            fails.append('%s: %s ends 0x%08X, past the boundary; reached by %s'
                         % (c, img.describe(f), f + img.size(f), chain(img, parent, f)))
        elif not start <= f < prev:
            fails.append('%s: %s is not in the groups (fast only by link order); reached by %s'
                         % (c, img.describe(f), chain(img, parent, f)))
    for c in ROOTS:
        print('  reached %-19s %6d bytes of code' % (c, by[c]))

    # 3. hot read-only data
    hot = hot_data(img, cls)
    for d, c in sorted(hot.items()):
        if d + img.size(d) > zw:
            fails.append('%s data: %s, %d bytes, ends 0x%08X, past the boundary'
                         % (c, img.describe(d), img.size(d), d + img.size(d)))
        elif not start <= d < prev:
            fails.append('%s data: %s is not in the groups (fast only by link order)' % (c, img.describe(d)))
    print('  hot read-only data  %6d bytes; left out: %s'
          % (sum(img.size(d) for d in hot), ', '.join(sorted(DATA_LEFT_OUT))))

    # 4. the names are current
    objs, funcs, data = listed(a.groups)
    placed = {o for (s, n, o) in img.inputs if start <= s < prev}
    for o in sorted(objs - placed):
        fails.append('the group list names %s, which is not in the groups (renamed or gone?)' % o)
    for n in sorted(funcs | data | set(STOP) | set(DATA_LEFT_OUT)):
        if n not in img.addr:
            fails.append('%s is named in the group list, STOP or DATA_LEFT_OUT but is not in the image' % n)

    for f in fails:
        print('FAIL: ' + f)
    print('check_zero_wait: %s' % ('OK' if not fails else '%d failure(s)' % len(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
