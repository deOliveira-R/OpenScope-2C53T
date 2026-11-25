# Settings: built-in defaults, user settings, reset, profiles

## Two things with two names

| | what it is | where it lives | who changes it |
|---|---|---|---|
| **Built-in defaults** | the value each property has on a device that has never saved anything | the property table, `firmware/src/model/model_table.h`, column `def` (const, in flash) | a build (see "Build-time defaults" below); never a running device |
| **User settings** | what this device has saved | the settings record in the W25Q settings region (`settings_store.c`, `config.c`) | every change of a saved property, by key or by shell |

At boot the user settings win; where there is no record, or a field of it is out of range, the built-in
default is used. A property is one of: saved in the user settings (`P` in `list`), session only (`S`: back to
its start value at every boot), user calibration (`U`: its own record, `cal user save`), or read only.

**Startup on Boot = Restore** adds one thing on top: the **session snapshot**. An orderly power-off writes the
value of every settable property (the `P` ones again and the `S` ones) to a log of its own in the W25Q
(`session`, two sectors, 0xF20000 and 0xF6F000; `firmware/src/util/session_store.h`), and the next boot, if Startup is
Restore, sets those values through the normal setters once the input task runs. Left out, each with a reason
(`session list` prints them): calibration, the generator's output enable (it keeps `gen.bootout`), hold and
relative, `view.run` (the scope always comes back running) and the
screen binding (the record's last screen). With any other Startup choice, or with no usable snapshot (none
yet, cleared, damaged, another format, or older than the settings record), the boot is the settings record
alone, as before. "Reset to defaults" invalidates the snapshot. `session status` says what the boot found and
which properties it could not put in force; the settings record itself (60 bytes, v5) is unchanged.

Calibration is neither of the two. Nothing on this page touches the factory calibration page (MCU flash
0x08006000), its W25Q backup, the user calibration record, the gallery, the boot logo or the firmware cache.

## Seeing the difference (shell)

| command | prints |
|---|---|
| `describe <prop>` | `default: ... (built in)`, and for a settable property `reset: back to the default` or `reset: kept (<why>)` |
| `list changed [prefix\|glob]` | only the properties whose value is not the built-in default: the value, the default, and `(reset keeps it: <why>)` where a reset would not put it back; last line `N differ ...; reset settings CONFIRM would put back M` |
| `get changed` | the same set as `get` lines (`name=value`), i.e. a profile of the differences |

`list` and `get <prop>` are unchanged (no marker in the lines scripts read): the word `changed` selects the
view. No property is named `changed` (every name has a dot).

## Reset to defaults

Copies the built-in defaults over the user settings. It replaces the old, inert "Factory Reset" row.

Two doors, one function (`settings_store_reset_to_defaults`, run by the input task):

- **Settings > Reset to Defaults.** OK asks `Reset all settings?  OK = yes   MENU = no`; a second OK resets,
  MENU goes back to the list, any other key leaves the question up. A single press never resets
  (`ui/settings_reset.h`). After the reset a popup says `Settings reset` and the screen stays in Settings.
  `ui screen` shows the question as `settings=8/<row>/0`.
- **Shell: `reset settings CONFIRM`.** The word is the confirm step; without it the command prints its usage.

What it does, in order:

1. Refuses while self-cal (autoset, baseline correction) runs: `BUSY`, nothing changed, nothing saved.
2. Every property a reset covers goes back to its default **through its setter**, in table order, so the
   rules run and the hardware follows: ranges and coupling relays, positions, timebase / roll, trigger source
   and level, view, persistence off, generator settings and output off, meter function, backlight, auto-off
   timer, theme. Only a value that is not already the default is set. A hardware path that is not live on the
   screen shown takes the stored value when it is next entered (the model's "stored" answer). A property that
   a rule or an owner refused is tried once more after the others; what is still not at its default is
   reported (`NOT_SET ... not at default: <names>`) and saved as it is.
   The remembered math operator (kept while math is off) goes back to A+B too.
3. The record: its image starts again from the defaults record, the last-screen byte is cleared, the live
   state is captured over it and it is written at once. When every property reached its default the record
   is byte for byte the one a device without a record would build.

After a reset `sys.startup` is `restore` and there is no last screen, so the next boot shows Home until an
orderly power-off (POWER hold, auto-off) names a screen again; the meaning of that byte is unchanged.
`meter.link` goes to its default like any other setting.

Boot loads the record differently (the store hooks write the variables and each owner's init pushes the
hardware). Those inits cannot run again on a running device, so the reset uses the setters.

What a reset keeps (`prop_reset_keeps`, shown by `describe` and in the reset's reply):

| kept | why |
|---|---|
| `il.gain`, `il.offset`, `il.skew`, `il.knots` | user calibration |
| `sys.instrument` (and its alias `sys.mode`) | the screen binding, not a setting: the screen stays where it is |
| `view.run` | only the OK key can apply RUN/STOP today (its setter is not wired) |
| read-only properties | state, not settings |

Proof on the host: `make test-settings-reset` (`tests/test_settings_reset.c`). It iterates the table (every
covered property at its default, every kept one unchanged), reads the owner calls from a fake-ops log,
compares the record with the defaults record byte by byte, and runs all of it against a NOR model of the
whole W25Q in which every program and erase outside the settings region is counted and every other region is
compared with a snapshot, a log compaction included. The MCU's own flash is not in that model. The code under
test (the model, its setters, the record codec, the store, the region layer) links without the files that can
write it or the gallery, logo, cal backup, user calibration and firmware cache, so it cannot call them. The
hardware ops are fakes in the test; the real ones and the owner functions they call were read by hand for
such calls (none).

### Not built: defaults for one session

"Start with defaults" (run a session on the built-in defaults, leave the user settings alone) is not
available, neither as a key held at power-on nor as `reset settings SESSION` (the command says so). The
settings store compares the live state with the saved image after every handled key, so the first key press
after such a reset would start a write of the defaults. It needs a hold in the store first: no write until
the next boot, with the power-off flush writing only the last-screen byte. That is a decision about what a
change made during such a session means, so it is left open.

## Readable profiles on the host: `scripts/model_config.py`

A profile is a JSON file of settings, each value exactly as the shell prints and accepts it:

```json
{
  "format": "openscope-profile/1",
  "name": "bench",
  "source": "device socket://127.0.0.1:7321, Build: ...",
  "settings": { "ch1.vdiv": "3", "ch1.coupling": "DC", "gen.amp": "3.0 V", "ch1.meas": "Freq,Vp-p" },
  "labels":   { "ch1.vdiv": "1.00V" }
}
```

It carries the saved settings only; never calibration, read-only state or aliases.

| command | does |
|---|---|
| `model_config.py list [glob]` | the table from the firmware source: name, id, type, range, built-in default, where it is saved, flags |
| `model_config.py defaults -o profiles/default.json` | the built-in defaults as a profile |
| `model_config.py from-device --port P -o my.json [--name N] [--session]` | one `get *` on the device; the saved properties (with `--session` the session ones too) into a profile |
| `model_config.py to-device my.json --port P [--dry-run]` | `set` each setting, in table order; prints every value the device refused, clamped or stored for later, then reads all back and lists what differs; exit 1 if anything was refused or differs |
| `model_config.py diff my.json [other.json]` | the profile against the built-in defaults, or against another profile |
| `model_config.py build my.json -o header.h` | the profile as build-time defaults (the Makefile calls it) |
| `model_config.py --self-test` | no device: see below |

`--port` is a serial device (needs pyserial) or a pyserial URL; `socket://HOST:PORT` (the broker) works
without pyserial.

`make test-model-config` holds the tool's reading of the table against the firmware model's own default
lines (the host build of the model prints them), `profiles/default.json` against the table, the header a
profile build would use against the model compiled with it, and every command against a fake shell,
in-process and over a socket on an ephemeral port.

## Build-time defaults: `make DEFAULTS=profiles/<name>.json`

```sh
make DEFAULTS=../profiles/lab.json                       # from firmware/ (the path is relative to it)
make app-coldtrace-meter DEFAULTS=../profiles/lab.json
```

`model_config.py build` checks the profile against the table (names, ranges; saved properties only: a
session property's boot value is its owner's initialiser, not the table's) and writes
`build/model_profile.h`. `model.c` takes every property's default from it, and `version` prints
`Defaults profile: <name> (N built-in defaults changed)`. No other file sees the header. A saved record still
wins over these defaults; "Reset to Defaults" goes back to the profile's.

Without `DEFAULTS` the image is byte for byte what it is without this hook.

The variable is `DEFAULTS`, not `PROFILE`: `PROFILE=bench` is the **feature** profile (`features.mk`), which
chooses what code is in the image. This one chooses default values.

## The state-owner ratchet

See [owners.md](owners.md): `make check-state-owners`.
