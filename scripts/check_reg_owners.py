#!/usr/bin/env python3
"""check_reg_owners.py - one owner per peripheral register (OpenScope ARCH round, rule (a)).

Scans firmware/src/**/*.{c,h} (not attic/, vendor libs) for WRITES to peripheral
registers and fails if a peripheral is written outside its owner(s).

What counts as a write (every assignment form: = |= &= ^= += -= *= /= %= <<= >>=, and ++/-- before or after):
  1. struct access on an instance:      GPIOC->scr = ..., TMR7->pr = ..., DAC->ctrl |= ..., GPIOC->odt++
  2. struct access through an alias:    #define FPGA_SPI SPI3   /  ((spi_type *)SPI2_BASE)
  3. struct access through a typed ptr: gpio_type *port = ...; port->clr = ...
  4. raw address writes:                *(volatile uint32_t *)0x40007400 = ...,  *((uint32_t *)0x40007400) = ...
                                        #define DAC_CTRL (*(volatile uint32_t *)0x40007400)  then DAC_CTRL = ...
  5. vendor-library / CMSIS write calls: gpio_init(GPIOB, ...), tmr_channel_value_set(TMR10, ...),
                                        wdt_counter_reload(), NVIC_EnableIRQ(...), flash_sector_erase(...)
  Any of these in a #define body counts where the macro is defined (function "#define NAME").
Register reads are never violations.

Owners:      scripts/reg_owners.txt     "<PERIPH>  <file>[:<function>] ..."   (PERIPH may be GPIOC.cfg, GPIOC.out, ...)
Exemptions:  a line comment containing  REG-OWNER-EXEMPT(<reason>)  (reviewed, e.g. pre-scheduler boot, RAM installer)
Baseline:    scripts/reg_owners_baseline.txt  "<file>\t<function>\t<PERIPH>\t<count>" - known violations.
             Fails on any violation not covered by the baseline (new file/function/periph, or a higher count).
             --update-baseline rewrites it; a stale baseline entry (fixed) is reported and, with --strict, fails,
             so the baseline only ever shrinks.
No-write dirs: a row "src/<dir>/  -" (e.g. src/model/) means nothing in that directory may write a register;
             REG-OWNER-EXEMPT is not honoured there.
--self-test: runs the checker on injected snippets (each write form, a macro body, a header, an exemption in a
             no-write dir) and on clean controls; exit 1 if any is misjudged. `make check-owners` runs it first.
Exit: 0 ok, 1 violations, 2 usage.
"""
import os, re, sys, argparse, collections, tempfile

INSTANCE = r'(?:GPIO[A-F]|TMR\d{1,2}|DAC|ADC[123]|DMA[12](?:_CHANNEL[1-7])?|SPI[1-4]|USART[1-3]|UART[4-8]|' \
           r'CRM|FLASH|WDT|WWDT|EXINT|IOMUX|XMC_BANK\w*|OTGFS1|USBD|PWC|BPR|ERTC|SCB|SysTick|NVIC|CAN[12]|I2C[12]|SDIO1?|CRC)'
PTR_TYPES = r'(?:gpio_type|tmr_type|dma_channel_type|dma_type|spi_type|usart_type|dac_type|adc_type|exint_type|crm_type)'
ASSIGN = r'\s*(?:\[[^\]]*\])?\s*(?:((?:<<|>>|[-+*/%|&^])?=)(?!=)|(\+\+|--))'
PREINC = r'(?:\+\+|--)\s*'
# *(volatile uint32_t *)0x...  /  *((uint32_t *)0x...)  /  (*(volatile uint16_t *)(0x...))
RAWADDR = r'\*\s*\(\s*\(?\s*(?:volatile\s+)?(?:const\s+)?uint(?:8|16|32)_t\s*(?:volatile\s*)?\*\s*\)\s*\(?\s*(0x[0-9A-Fa-f]+)[uUlL]*\s*\)?\s*\)?'

# raw address -> peripheral (AT32F403A memory map, RM 2.2)
ADDR_MAP = [
    (0x40000000, 0x40000400, 'TMR2'), (0x40000400, 0x40000800, 'TMR3'), (0x40000800, 0x40000C00, 'TMR4'),
    (0x40000C00, 0x40001000, 'TMR5'), (0x40001000, 0x40001400, 'TMR6'), (0x40001400, 0x40001800, 'TMR7'),
    (0x40001800, 0x40001C00, 'TMR12'), (0x40001C00, 0x40002000, 'TMR13'), (0x40002000, 0x40002400, 'TMR14'),
    (0x40003000, 0x40003400, 'WDT'), (0x40003800, 0x40003C00, 'SPI2'), (0x40003C00, 0x40004000, 'SPI3'),
    (0x40004400, 0x40004800, 'USART2'), (0x40006C00, 0x40007000, 'BPR'), (0x40007000, 0x40007400, 'PWC'),
    (0x40007400, 0x40007800, 'DAC'), (0x40010000, 0x40010400, 'IOMUX'), (0x40010400, 0x40010800, 'EXINT'),
    (0x40010800, 0x40010C00, 'GPIOA'), (0x40010C00, 0x40011000, 'GPIOB'), (0x40011000, 0x40011400, 'GPIOC'),
    (0x40011400, 0x40011800, 'GPIOD'), (0x40011800, 0x40011C00, 'GPIOE'), (0x40012400, 0x40012800, 'ADC1'),
    (0x40014C00, 0x40015000, 'TMR10'), (0x40015000, 0x40015400, 'TMR11'), (0x40020000, 0x40020400, 'DMA1'),
    (0x40020400, 0x40020800, 'DMA2'), (0x40021000, 0x40021400, 'CRM'), (0x40022000, 0x40022400, 'FLASH'),
    (0x60000000, 0x70000000, 'LCDBUS'), (0xA0000000, 0xA0001000, 'XMC'), (0xE000E000, 0xE000F000, 'SCS'),
]
# vendor library / CMSIS calls that write a peripheral: regex -> peripheral (None = from the first argument)
# Vendor-library write calls: every function the AT32 driver headers declare, minus pure getters/initialisers
# of RAM structs. Loaded from --lib-inc so our own functions (dac_output_init, usart_tx_wire_live) never match.
LIB_READONLY = re.compile(r'(_get|_read|_status|default_para_init|_flag_get|_interrupt_flag_get)$')
LIB_PERIPH = {'gpio_pin_remap_config': 'IOMUX', 'gpio_exint_line_config': 'IOMUX', 'nvic_irq_enable': 'NVIC',
              'nvic_irq_disable': 'NVIC', 'nvic_priority_group_config': 'NVIC'}
CMSIS_WRITES = {'NVIC_EnableIRQ': 'NVIC', 'NVIC_DisableIRQ': 'NVIC', 'NVIC_SetPriority': 'NVIC',
                'NVIC_SystemReset': 'SCS', 'SysTick_Config': 'SCS'}
def load_lib(inc_dir):
    names = {}
    for f in sorted(os.listdir(inc_dir)):
        if not f.endswith('.h'): continue
        for m in re.finditer(r'^\s*[a-z_][\w \*]*?\b([a-z][a-z0-9]*_\w+)\s*\(', open(os.path.join(inc_dir, f), errors='replace').read(), re.M):
            fn = m.group(1)
            if not LIB_READONLY.search(fn): names[fn] = LIB_PERIPH.get(fn)
    names.update(CMSIS_WRITES)
    return names
EXEMPT_RE = re.compile(r'REG-OWNER-EXEMPT\(([^)]+)\)')

FUNC_RE = re.compile(r'^(?!\s*(?:if|for|while|switch|return|else)\b)[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)\s*\([^;]*$')
EXEMPT_RE = re.compile(r'REG-OWNER-EXEMPT\(([^)]+)\)')

def periph_of_addr(a):
    for lo, hi, name in ADDR_MAP:
        if lo <= a < hi:
            return name
    return 'RAW@%08X' % a

def gpio_class(periph, field):
    """GPIO split: .cfg (CFGLR/CFGHR, read-modify-write, racy) vs .out (SCR/CLR/ODT)."""
    if periph.startswith('GPIO') and len(periph) == 5:
        return periph + ('.cfg' if field.startswith('cfg') else '.out')
    return periph

def strip_comments(lines):
    out, inc = [], False
    for raw in lines:
        s = raw
        if inc:
            if '*/' in s:
                s = s.split('*/', 1)[1]; inc = False
            else:
                out.append(('', raw)); continue
        s = re.sub(r'/\*.*?\*/', ' ', s)
        if '/*' in s:
            s = s.split('/*', 1)[0]; inc = True
        s = s.split('//', 1)[0]
        s = re.sub(r'"(?:\\.|[^"\\])*"', '""', s)     # strings never hold writes
        out.append((s, raw))
    return out

def scan_file(path, rel, aliases_global, LIB):
    text = open(path, errors='replace').read().split('\n')
    lines = strip_comments(text)
    aliases = dict(aliases_global)   # NAME -> periph (struct alias) or ('raw', periph)
    ptrvars = {}
    for code, _ in lines:
        m = re.match(r'\s*#\s*define\s+(\w+)\s+\(?\s*\(\s*' + PTR_TYPES + r'\s*\*\s*\)\s*(\w+?)_BASE\s*\)?', code)
        if m: aliases[m.group(1)] = m.group(2)
        m = re.match(r'\s*#\s*define\s+(\w+)\s+(' + INSTANCE + r')\s*$', code)
        if m: aliases[m.group(1)] = m.group(2)
        m = re.match(r'\s*#\s*define\s+(\w+)(?:\([^)]*\))?\s+\(\s*\*\s*\(\s*(?:volatile\s+)?uint(?:8|16|32)_t\s*(?:volatile\s*)?\*\s*\)\s*\(?\s*(0x[0-9A-Fa-f]+)', code)
        if m: aliases[m.group(1)] = ('raw', periph_of_addr(int(m.group(2).rstrip('uUlL'), 16)))
        for pm in re.finditer(PTR_TYPES + r'\s*\*\s*(\w+)\s*=\s*(?:\([^)]*\)\s*)?(' + INSTANCE + r'|\w+)?', code):
            ptrvars[pm.group(1)] = pm.group(2) if pm.group(2) and re.fullmatch(INSTANCE, pm.group(2)) else '?PTR'
        for pm in re.finditer(r'\b' + PTR_TYPES + r'\s*\*\s*(\w+)\s*[,)]', code):   # parameters
            ptrvars.setdefault(pm.group(1), '?PTR')
        for pm in re.finditer(r'volatile\s+uint(?:8|16|32)_t\s*\*\s*(\w+)\s*=\s*\(\s*volatile\s+uint(?:8|16|32)_t\s*\*\s*\)\s*\(?\s*(?:\w+\s*\?\s*)?(0x[0-9A-Fa-f]+)', code):
            ptrvars[pm.group(1)] = ('raw', periph_of_addr(int(pm.group(2).rstrip('uUlL'), 16)))
    hits = []
    func, depth = '<file>', 0
    for n, (code, raw) in enumerate(lines, 1):
        if depth == 0:
            fm = FUNC_RE.match(code)
            if fm and not code.lstrip().startswith('#'):
                func = fm.group(1)
        depth = max(0, depth + code.count('{') - code.count('}'))
        where = func
        if code.lstrip().startswith('#'):
            # A macro body is scanned where it is defined (its uses are not expanded). Other directives hold no writes.
            dm = re.match(r'\s*#\s*define\s+(\w+)(?:\([^)]*\))?(.*)$', code)
            if not dm:
                continue
            where, code = '#define ' + dm.group(1), dm.group(2)
        ex = EXEMPT_RE.search(raw)
        def add(periph, what):
            hits.append((rel, n, where, periph, what, ex.group(1) if ex else None, raw.strip()))
        struct_w = r'\b(' + INSTANCE + r'|\w+)->([a-z_0-9]+)(?:_bit\.\w+|\.\w+)?'
        sites = [m for m in re.finditer(struct_w + ASSIGN, code)]
        sites += [m for m in re.finditer(PREINC + struct_w, code)]
        for m in sites:
            base, field = m.group(1), m.group(2)
            if re.fullmatch(INSTANCE, base): add(gpio_class(base, field), 'struct')
            elif base in aliases and not isinstance(aliases[base], tuple): add(gpio_class(aliases[base], field), 'alias')
            elif base in ptrvars:
                p = ptrvars[base]
                add(gpio_class(p, field) if isinstance(p, str) else p[1], 'ptr')
        for m in list(re.finditer(RAWADDR + ASSIGN, code)) + list(re.finditer(PREINC + r'\(?\s*' + RAWADDR, code)):
            add(periph_of_addr(int(m.group(1), 16)), 'raw')
        rawname = r'(?<![\w>.])(\*\s*)?(\w+)(?:\([^)]*\))?'
        for m in list(re.finditer(rawname + ASSIGN, code)) + list(re.finditer(PREINC + rawname, code)):
            name = m.group(2)
            if re.search(r'(?:_t|int|char|volatile|const)\s*$', code[:m.start()]):   # a declaration, not a store
                continue
            if m.group(1) and name in ptrvars and isinstance(ptrvars[name], tuple): add(ptrvars[name][1], 'rawptr')
            elif not m.group(1) and name in aliases and isinstance(aliases[name], tuple): add(aliases[name][1], 'rawmacro')
        for m in re.finditer(r'\b([A-Za-z_]\w*)\s*\(\s*&?\s*([A-Za-z_0-9]*)', code):
            fn, arg = m.group(1), m.group(2)
            if fn not in LIB: continue
            periph = LIB[fn]
            if periph is None:
                a = aliases.get(arg, arg)
                if isinstance(a, str) and re.fullmatch(INSTANCE, a): periph = a
                elif isinstance(ptrvars.get(arg), str): periph = ptrvars[arg]
                else: periph = '?PTR' if fn.startswith(('gpio_', 'tmr_')) else fn.split('_')[0].upper()
                if fn.startswith('gpio_') and re.fullmatch(r'GPIO[A-F]', periph or ''):
                    periph += '.cfg' if fn == 'gpio_init' else '.out'
            add(periph, 'lib:' + fn)
        if re.search(r'\b(?:__disable_irq|__set_PRIMASK|taskDISABLE_INTERRUPTS)\s*\(', code):
            add('PRIMASK', 'irqmask')
    return hits

def load_owners(path):
    owners, key = collections.defaultdict(list), None
    for raw in open(path):
        line = raw.split('#', 1)[0]
        if not line.strip(): continue
        tok = line.split()
        if line[0].isspace() and key:          # continuation line
            owners[key].extend(tok)
        else:
            key = tok[0]; owners[key].extend(tok[1:])
    return owners

def owned(owners, periph, rel, func):
    keys = [periph, periph.split('.')[0] + '.*', re.sub(r'^GPIO[A-F]', 'GPIO*', periph), 'GPIO*.*' if periph.startswith('GPIO') else periph]
    for k in keys:
        for o in owners.get(k, []):
            f, _, fn = o.partition(':')
            if (rel == f or (f.endswith('/') and rel.startswith(f))) and (not fn or fn == func):
                return True
    return False

def collect_hits(src, lib):
    skip = ('attic', 'at32f403a_lib', 'FreeRTOS', 'CMSIS', 'CMSIS-DSP')
    hits = []
    for d, dirs, fs in os.walk(src):
        dirs[:] = sorted(x for x in dirs if x not in skip)
        for f in sorted(fs):
            if f.endswith(('.c', '.h')):
                p = os.path.join(d, f)
                hits += scan_file(p, os.path.relpath(p, src), {}, lib)
    return hits

def violations(hits, owners):
    viol = collections.Counter()
    # "src/model/  -" rows: a directory that owns no peripheral. Any write in it is a violation, whoever owns the
    # register, and an exemption does not apply there.
    nowrite = [k[len('src/'):] for k, v in owners.items() if k.startswith('src/') and k.endswith('/') and v == ['-']]
    for rel, n, func, periph, what, ex, raw in hits:
        if any(rel.startswith(d) for d in nowrite):
            viol[(rel, func, periph)] += 1
            continue
        if ex or owned(owners, periph, rel, func):
            continue
        viol[(rel, func, periph)] += 1
    return viol

# (file, code, must be flagged). The owners below: drivers/own.c owns GPIO*, drivers/dac.c owns DAC.
SELF_OWNERS = 'src/model/   -\nGPIO*.*  drivers/own.c\nDAC  drivers/dac.c\n'
SELF_CASES = [
    ('drivers/x.c', 'void f(void) { GPIOC->odt++; }', True),
    ('drivers/x.c', 'void f(void) { ++GPIOC->odt; }', True),
    ('drivers/x.c', 'void f(void) { GPIOC->odt--; }', True),
    ('drivers/x.c', 'void f(void) { GPIOC->odt <<= 1; }', True),
    ('drivers/x.c', 'void f(void) { GPIOC->odt >>= 1; }', True),
    ('drivers/x.c', 'void f(void) { GPIOC->odt += 2; }', True),
    ('drivers/x.c', 'void f(void) { GPIOC->odt -= 2; }', True),
    ('drivers/x.c', 'void f(void) { GPIOC->odt *= 2; }', True),
    ('drivers/x.c', '#define W() do { GPIOC->scr = 1; } while (0)', True),
    ('drivers/x.c', '#define W() \\\n    do { GPIOC->scr = 1; } while (0)', True),
    ('drivers/x.c', '#define LED() gpio_bits_set(GPIOC, 1)', True),
    ('model/model.c', 'void f(void) { GPIOC->scr = 1; /* REG-OWNER-EXEMPT(x) */ }', True),
    ('model/model.h', 'static inline void f(void) { GPIOC->scr = 1; }', True),
    ('drivers/x.h', 'static inline void f(void) { DAC->ctrl |= 1; }', True),
    ('drivers/x.c', 'void f(void) { *((uint32_t *)0x40007400) = 1; }', True),
    ('drivers/x.c', 'void f(void) { (*(volatile uint32_t *)(0x40007400)) |= 1; }', True),
    ('drivers/x.c', 'void f(void) { ++*(volatile uint32_t *)0x40007400; }', True),
    ('drivers/x.c', '#define R (*(uint32_t *)0x40007400)\nvoid f(void) { R = 1; }', True),
    ('drivers/x.c', '#define R (*(volatile uint32_t *)0x40007400)\nvoid f(void) { R++; }', True),
    # already caught before: struct, alias, typed pointer, raw volatile, library call, model/ write
    ('drivers/x.c', 'void f(void) { GPIOC->scr = 1; }', True),
    ('drivers/x.c', '#define P GPIOC\nvoid f(void) { P->clr = 1; }', True),
    ('drivers/x.c', 'void f(gpio_type *g) { g->clr = 1; }', True),
    ('drivers/x.c', 'void f(void) { *(volatile uint32_t *)0x40007400 = 1; }', True),
    ('drivers/x.c', 'void f(void) { gpio_bits_set(GPIOC, 1); }', True),
    ('model/model.c', 'void f(void) { DAC->ctrl = 1; }', True),
    # clean: the owner, reads, comparisons, an exemption outside a no-write dir, a plain counter
    ('drivers/own.c', 'void f(void) { GPIOC->odt++; GPIOC->scr = 1; }', False),
    ('drivers/own.c', '#define W() do { GPIOC->scr = 1; } while (0)', False),
    ('drivers/x.c', 'int f(void) { int a = GPIOC->idt; if (GPIOC->odt >= 1 || GPIOC->odt <= 2) a++; '
                    'return a + (GPIOC->odt == 3) + (GPIOC->odt != 4); }', False),
    ('drivers/x.c', 'void f(void) { GPIOC->scr = 1; /* REG-OWNER-EXEMPT(reviewed boot) */ }', False),
    ('drivers/x.c', 'static int n; void f(void) { n++; ++n; n <<= 1; }', False),
    ('drivers/x.c', '#define LIMIT 5\n#include <stdint.h>', False),
    ('model/model.c', 'int f(void) { return GPIOC->idt; }', False),
]

def self_test(lib_inc):
    lib = load_lib(lib_inc) if os.path.isdir(lib_inc) else dict(CMSIS_WRITES, gpio_bits_set=None)
    bad = 0
    with tempfile.TemporaryDirectory() as td:
        owners_p = os.path.join(td, 'owners.txt')
        with open(owners_p, 'w') as fh: fh.write(SELF_OWNERS)
        owners = load_owners(owners_p)
        for i, (rel, code, want) in enumerate(SELF_CASES):
            src = os.path.join(td, 'src%d' % i)
            os.makedirs(os.path.join(src, os.path.dirname(rel)))
            with open(os.path.join(src, rel), 'w') as fh: fh.write(code + '\n')
            got = sum(violations(collect_hits(src, lib), owners).values()) > 0
            if got != want:
                bad += 1
                print('SELF-TEST FAIL (%s expected, %s): %s: %s' % ('flagged' if want else 'clean',
                      'flagged' if got else 'clean', rel, code.replace('\n', ' | ')))
    print('reg owners self-test: %d cases, %d misjudged' % (len(SELF_CASES), bad))
    return 1 if bad else 0

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', default='firmware/src')
    ap.add_argument('--lib-inc', default='firmware/at32f403a_lib/libraries/drivers/inc')
    ap.add_argument('--owners', default='scripts/reg_owners.txt')
    ap.add_argument('--baseline', default='scripts/reg_owners_baseline.txt')
    ap.add_argument('--update-baseline', action='store_true')
    ap.add_argument('--strict', action='store_true')
    ap.add_argument('--census', action='store_true', help='print every write, owned or not')
    ap.add_argument('--self-test', action='store_true', help='check the checker on injected snippets')
    a = ap.parse_args()
    if a.self_test:
        return self_test(a.lib_inc)
    lib = load_lib(a.lib_inc)
    hits = collect_hits(a.src, lib)
    if a.census:
        for h in hits: print('\t'.join(str(x) for x in h))
        return 0
    owners = load_owners(a.owners)
    viol = violations(hits, owners)
    if a.update_baseline:
        with open(a.baseline, 'w') as fh:
            for k in sorted(viol): fh.write('%s\t%s\t%s\t%d\n' % (k + (viol[k],)))
        print('baseline: %d entries, %d writes' % (len(viol), sum(viol.values())))
        return 0
    base = collections.Counter()
    if os.path.exists(a.baseline):
        for line in open(a.baseline):
            p = line.rstrip('\n').split('\t')
            if len(p) == 4: base[tuple(p[:3])] = int(p[3])
    new = {k: v for k, v in viol.items() if v > base.get(k, 0)}
    stale = {k: v for k, v in base.items() if viol.get(k, 0) < v}
    for (rel, func, periph), v in sorted(new.items()):
        print('NEW  %-30s %-36s %-12s x%d (baseline %d)' % (rel, func, periph, v, base.get((rel, func, periph), 0)))
    for (rel, func, periph), v in sorted(stale.items()):
        print('FIXED %-29s %-36s %-12s baseline %d now %d: run --update-baseline' % (rel, func, periph, v, viol.get((rel, func, periph), 0)))
    print('reg owners: %d writes scanned, %d outside owners (%d baselined), %d new, %d fixed'
          % (len(hits), sum(viol.values()), sum(base.values()), len(new), len(stale)))
    return 1 if new or (a.strict and stale) else 0

if __name__ == '__main__':
    sys.exit(main())
