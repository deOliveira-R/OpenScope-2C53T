# Register owners: one owner per peripheral

## The rule

Every peripheral register has one owner: a driver file (or a single function) that is allowed to write it.
Other code asks the owner to do the write. This matters because several tasks and interrupts run on the
device; two of them writing the same register (especially read-modify-write ones like `USART2->ctrl1 |= ...`
or a GPIO configuration nibble) can undo each other's change.

Reads are never a problem. Only writes count: struct stores (`GPIOC->scr = ...`), stores through aliases and
typed pointers, raw-address stores (volatile or not), calls into the vendor driver library that change hardware
(`gpio_init`, `tmr_channel_value_set`, `wdt_counter_reload`, ...), CMSIS `NVIC_*` calls and `__disable_irq`.
Every assignment form counts (`=`, `|=`, `&=`, `^=`, `+=`, `-=`, `*=`, `/=`, `%=`, `<<=`, `>>=`, and `++`/`--`
before or after). Both `.c` and `.h` files are scanned (inline functions in headers count). A write in a
`#define` body counts where the macro is defined, under the function name `#define NAME`; the uses of the macro
are not expanded.

`src/model/` owns no peripheral at all. It holds user-settings state only and reaches hardware through the
owner drivers, so any write there is a violation, whoever owns the register. `REG-OWNER-EXEMPT` does not apply
in a no-write directory.

## Files

- `scripts/check_reg_owners.py`: the checker.
- `scripts/reg_owners.txt`: the owner table. One line per peripheral: `PERIPH owner owner ...`. An owner is a
  file (relative to `firmware/src`), a directory ending in `/`, or `file:function`. GPIO is split per port and
  per register class: `GPIOC.cfg` (configuration, read-modify-write) and `GPIOC.out` (set/clear/output).
  The table already lists owner drivers that are planned but do not exist yet.
- `scripts/reg_owners_baseline.txt`: the known violations that exist today, one per line:
  `file<TAB>function<TAB>peripheral<TAB>count`. The refactor removes them one by one.

## Running it

From `firmware/`:

```sh
make check-owners
```

or from the repository root:

```sh
python3 -I scripts/check_reg_owners.py --src firmware/src \
    --lib-inc firmware/at32f403a_lib/libraries/drivers/inc
```

It works as a ratchet:

- `NEW ...` lines are writes outside their owner that the baseline does not cover (a new file, function or
  peripheral, or a higher count). Exit code 1. Fix the code: move the write into the owner, or call the
  owner's API.
- `FIXED ...` lines mean a baselined violation is gone. This is good news; run `--update-baseline` in the same
  commit so the baseline shrinks. With `--strict` a `FIXED` line also fails.
- Exit 0 means no new violations.

Useful options: `--census` prints every write found, owned or not (file, line, function, peripheral).
`--self-test` runs the checker on injected snippets (each write form, a macro body, a header, an exemption in
`src/model/`, non-volatile raw casts) and on clean controls (the owner, reads, comparisons, plain counters), and
fails if any is misjudged. `make check-owners` runs it before the scan.

## Updating the baseline

Only to record a fix (the numbers go down), never to hide a new violation:

```sh
python3 -I scripts/check_reg_owners.py --update-baseline
git diff scripts/reg_owners_baseline.txt   # review: only removals or lower counts
```

Commit the baseline together with the code change that fixed the writes. If a change adds an owner driver,
add its row to `scripts/reg_owners.txt` in the same commit.

## Exemptions

A line comment `REG-OWNER-EXEMPT(<reason>)` on the write line excludes it, for reviewed cases such as the
pre-scheduler boot sequence or the RAM flash installer. Give a real reason. It is ignored in a no-write
directory (`src/model/`).

## Blind spots

The scanner is text-based. It cannot see addresses computed at run time or pointer-typed address macros (for
example the GPIOD/GPIOE address built inside `main.c`'s `_GPIO_CFG`); those are reviewed by hand. A macro is
judged where it is defined, not where it is used: a write macro defined in an owner file and used elsewhere is
not seen at the use.

## The sibling ratchet: state owners

The same idea one level up. Every user setting is a property (`firmware/src/model/model_table.h`) whose value
lives in a variable the firmware already had: a field of `scope_state_t`, a `main.c` global such as
`meter_submode`. The property's setter is the one place that may change it at run time, because the setter is
what applies the value through the owner driver, bumps the change epoch and notes the settings record. A store
anywhere else (`ss->timebase_idx = x` in a key handler or a shell command) changes the value behind the
model's back: nothing is applied, nothing repaints, nothing is saved, and `get` reports a value that is not in
force.

- `scripts/check_state_owners.py`: the checker. It counts stores (every assignment form, as above) to the
  fields of `scope_state_t`, `channel_state_t`, `trigger_state_t` and `cursor_state_t` that back a property,
  where the expression is known to be scope state (a pointer the function declared with the type, a helper
  that returns one, the member chains `->ch1.`, `->trigger.`, `->cursor.`), and to the globals the properties
  wrap. A definition with an initialiser at file scope is the built-in default, not a store. Its header lists
  what it does not see (whole-struct copies, a store through a pointer to the field, and owner modules that
  keep their state behind setter functions).
- `scripts/state_owners.txt`: who may store. `*  model/` (the setters and store hooks), `ui/scope_state.c`
  (the pure step helpers and the initialiser), and per state the owner function a setter reaches through the
  hardware ops (`sys.mode`: `current_mode` is stored by the screen router with its base, `ui/screen.c set_base_locked`; `view.mode` in `scope_view_select`, `chN.pos` in `scope_vpos_set`).
- `scripts/state_owners_baseline.txt`: the stores that exist today, `file<TAB>function<TAB>state<TAB>count`.

```sh
make check-state-owners          # from firmware/: the self-test, then the scan
python3 -I scripts/check_state_owners.py --top 10      # from the repository root: the files with the most
python3 -I scripts/check_state_owners.py --census      # every store, owned or not
```

`NEW` fails (exit 1): set the property instead, `prop_set_local()` on the input task, `prop_request()` or
`prop_post()` from another task. `FIXED` is good news: run `--update-baseline` in the same commit. A reviewed
exception is a line comment `STATE-OWNER-EXEMPT(<reason>)`.

## The gate

From `firmware/`, before a branch is merged: the images (`make app-coldtrace-meter`, `make`, `make app-full`,
`make emu`), `make test-all`, `make check-owners` and `make check-state-owners`; the two ratchets must report
`0 new`.
