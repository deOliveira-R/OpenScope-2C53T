# Register owners: one owner per peripheral

## The rule

Every peripheral register has one owner: a driver file (or a single function) that is allowed to write it.
Other code asks the owner to do the write. This matters because several tasks and interrupts run on the
device; two of them writing the same register (especially read-modify-write ones like `USART2->ctrl1 |= ...`
or a GPIO configuration nibble) can undo each other's change.

Reads are never a problem. Only writes count: struct stores (`GPIOC->scr = ...`), stores through aliases and
typed pointers, raw-address stores, calls into the vendor driver library that change hardware (`gpio_init`,
`tmr_channel_value_set`, `wdt_counter_reload`, ...), CMSIS `NVIC_*` calls and `__disable_irq`.

`src/model/` owns no peripheral at all. It holds user-settings state only and reaches hardware through the
owner drivers, so any write there is a violation, whoever owns the register.

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
pre-scheduler boot sequence or the RAM flash installer. Give a real reason.

## Blind spots

The scanner is text-based. It cannot see addresses computed at run time or pointer-typed address macros (for
example the GPIOD/GPIOE address built inside `main.c`'s `_GPIO_CFG`); those are reviewed by hand.
