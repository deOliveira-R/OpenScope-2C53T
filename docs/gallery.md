# On-device screenshots, gallery, USB Sharing and boot logo (parity C16)

Status 2026-11-11: **screenshot save, 3x3 gallery and the `screenshot` shell
command are implemented** (host-tested; bench test pending). **USB Sharing is
implemented** as a read-only mass-storage drive synthesised from the gallery
(host-tested, macOS mounts the generated image; bench test pending, see the
USB Sharing section; since 2026-11-11 the unit boots as the plain CDC device and
re-enumerates on request with an automatic fallback). The **boot-logo upload is
built** (last section; bench test pending).

## What stock does (V1.2.0)

- Short **SAVE** in the scope writes `2:/Screenshot file/%d.bmp` (path string
  `0x080BC1B4`) through FatFs onto its volume `2:` (W25Q `0x200000..0xFFFFFF`,
  FAT12, 4 KB sectors). Popups "Saving..." / "Saved" / "Save failed" are
  ms[0xE10] = 1/2/3 (renderer around `0x0801C7B0..0x0801CA08`).
- **Long SAVE** opens the gallery, a 3x3 "nine palace grid" with delete (manual).
- Boot creates `2:/LOGO`, `2:/Screenshot file`, `2:/Screenshot simple file`,
  `3:/System file` (`0x080357BC..0x080358EA`, `f_mkfs` on FR_NO_FILESYSTEM).
- **USB Sharing** (settings item, OK handler `0x0800BF08`, ms[0xF68] = 4) exposes
  volume `2:` as a mass-storage device; the manual's logo recipe copies
  `LOGO2C53T.jpg` into the drive's `LOGO` folder.
- Boot opens `2:/LOGO/LOGO2C53T.jpg` (`0x0802B420..0x0802B456`) and draws it
  with the backlight off, else the embedded logo (docs/re/parity2_static.md 4.6).

## Where OpenScope puts screenshots, and why not in stock's FAT

Everything OpenScope owns on the W25Q sits at `0xD00000+`, which stock's FAT
calls free space (`flash_regions.c`, "KNOWN TRADE-OFF"): the firmware cache, the
user-cal and settings logs, modules, scratch and the factory-cal backup
(`0xFFD000`). A FAT writer for `2:` would be the first OpenScope code able to
allocate those clusters to a file, i.e. to overwrite our own calibration
backup. So screenshots do **not** go into stock's FAT. They go into our own
`scratch` region through the bounds-checked region layer:

| range | what |
|---|---|
| `0xFA0000 + k * 0xA000`, k = 0..8 | slot k: 10 sectors (40 KB) |
| slot +0 | 24-byte slot header, written **last** (the commit record) |
| slot +256 | a complete 4-bpp BMP file, 38518 bytes |
| `0xFFA000..0xFFCFFF` | unused tail of scratch (12 KB) |
| `0xFFD000..0xFFEFFF` | `calbackup`, untouched (separate RW region) |

Slot header (little-endian): `+0` magic `"OSG1"`, `+4` version 1, `+6` header
length 24, `+8` seq, `+12` BMP length, `+16` CRC32 of the BMP, `+20` CRC32 of
bytes 4..19. The header CRC leaves out the magic on purpose: delete zeroes the
magic only (a 1 -> 0 program, no erase), so a deleted header still proves its
number and numbering stays monotonic.

The picture is the LCD's 4-bit indexed software shadow (`lcd.c`, the source of
`scripts/screenshot.py`); its 160-byte rows are already the BMP 4-bpp row format,
so a save needs no RAM buffer: the slot is erased sector by sector, the BMP
header and the shadow are written straight from RAM, every page is read back by
the region layer, then the header commits. Colours are the shadow's 16-colour
approximation, as for the USB screenshot. A power cut leaves a slot that reads
EMPTY (no header) or DAMAGED, and the next save erases and reuses it.

Files are named `<seq>.bmp` like stock's `%d.bmp`. Nine slots are one gallery
page. When all nine hold shots, SAVE says "Gallery full: delete" and writes
nothing; it never overwrites a shot the user has not deleted.

The same trade-off as the rest of `0xD00000+` applies: a **stock** firmware that
fills its volume past 10 MB could allocate scratch clusters and overwrite our
screenshots (never the reverse). The slot CRC catches that: the shot reads
DAMAGED and drops out of the gallery.

## Keys

| where | key | does |
|---|---|---|
| every mode (scope and its settings page, meter, signal generator, settings, home) | SAVE | save the screen; popup "Saved N.bmp" / "Gallery full: delete" / "Save failed" (UI round 1, owner decision D1; the meter's old SAVE = debug overlay is the shell's `meter overlay`) |
| every mode | long SAVE | open the gallery (3x3, newest first) |
| gallery grid | arrows | move; LEFT/RIGHT wrap |
| gallery grid | OK | view the selected shot full screen |
| gallery (both views) | SAVE | arm delete ("SAVE again: delete N.bmp"); SAVE again deletes; any other key disarms |
| full view | LEFT/RIGHT | previous/next shot; OK or MENU back to the grid |
| gallery grid | MENU, long SAVE | close |
| anywhere | POWER | closes the gallery, then the usual power-off countdown |

The capture freezes rendering (`ui_modal_active`, waiting for the display task to
acknowledge, `ui_modal_idle_count`) for the ~0.5 s write, so the popup is not in
the picture and no frame is half drawn.

## Shell

```
screenshot [list]        SHOTS count=.. slots=9 next=.. region=scratch 0xFA0000+0x5D000 slot=0xA000
                           <n>.bmp slot k @0x...... len=38518 crc32=XXXXXXXX
screenshot save          screenshot: saved <n>.bmp
screenshot get <n>       SHOTBIN name=<n>.bmp len=38518 crc32=XXXXXXXX\r\n <38518 raw bytes> \r\nSHOTBIN END crc32=XXXXXXXX
screenshot check <n>     screenshot: <n>.bmp stored=XXXXXXXX read=XXXXXXXX OK
screenshot delete <n>    screenshot: delete <n>.bmp: ok
usb [status]             USB: mode=cdc state=steady share=off last_switch=none switches=0 fallbacks=0 attach_ms=0
                         CDC: conf=1 tx_heals=0 rx_heals=0 tx_timeouts=0 tx_drops=0
                         MSC: conf=1 medium=in files=.. blocks=1024x512 ...   (only while mode=cdc+msc)
usb share [on|off]       usb: share on: re-enumerating as CDC + read-only drive. This port closes now ...
usb trace                usb trace: attach as CDC+MSC, N events / +  12 ms RESET / +  60 ms SETUP 80 06 wValue=0100 ...
```

`<n>` may be `3` or `3.bmp`.

## USB Sharing (mass storage)

Stock's Settings item "USB Sharing" (OK handler `0x0800BF08`, ms[0xF68] = 4)
re-enumerates as a mass-storage-only device, stops the scope, and exposes its
volume `2:` **writable**. Ours differs on purpose:

| | stock | OpenScope |
|---|---|---|
| what the PC sees | volume `2:` itself (FAT12, 4 KB sectors) | a FAT12 volume computed per sector from the gallery |
| writes | allowed | refused (MODE SENSE write-protect; WRITE -> DATA PROTECT / WRITE PROTECTED) |
| CDC shell | gone while sharing | kept: while sharing the device is CDC + MSC |
| enter / leave | re-enumeration | re-enumeration, with an automatic fallback to CDC-only |
| scope while sharing | stopped | keeps running |

**Boot = plain CDC; sharing = re-enumeration with a safety net**
(2026-11-11, `src/util/usb_mode_sm.h`). The unit always boots as the CDC-only
device: the AT32 library's `cdc_desc.c`, byte for byte the `USB_MSC=0` device
(class 02, bcdDevice 2.00, 2 interfaces). `usb share on` or Settings "USB
Sharing" -> On makes the shell task re-enumerate: 200 ms for the reply to
leave, D+ pull-up off for 600 ms (the host sees an unplug), then the composite
descriptors and pull-up on. If the host has not selected the configuration
within **3 s**, the unit detaches again and comes back as the CDC-only device by
itself; `usb` then reports `last_switch=FALLBACK ...` and `usb trace` lists the
host's requests during the failed attach (bus resets and every SETUP packet,
timestamped, with `-> STALL` where the device refused one). It does not retry by
itself; a new `usb share on` does. `usb share off` (or the Settings row) goes
back to CDC-only the same way. Sharing is off at every boot.

Why: on unit #2 (macOS, Apple silicon) the previous design, which BOOTED as the
composite device, enumerated with the right strings but was never configured
(IOUSBHostDevice `!registered, !matched`, no interfaces), and the shell was
gone until reflashing. A static check of everything the device sends found the
descriptors valid (`make test-usb` validates the exact bytes, 98-byte
configuration, against a strict checker; the CDC part is byte-identical to the
library's CDC-only descriptor) and the AT32 control path sound (EP0 multi-packet
IN for 98 bytes, wLength clamping, SET_CONFIGURATION opening EP1..EP3, packet
memory exactly 512 B without overlap). See "Root cause status" below.

**macOS port names.** macOS names a CDC ACM port `/dev/cu.usbmodem<serial><n>`
where `<serial>` is the USB serial string (here the 12 hex digits from the MCU
UID, e.g. `07B2D46E71C5`) and `<n>` comes from the CDC interface (here `1`):
unit #2 is `/dev/cu.usbmodem07B2D46E71C51`. The composite device keeps the
serial string and puts the CDC comm interface at 0, so the name is expected to
stay the same; because a switch is an unplug + replug, the port **closes** on
every switch (terminal programs report the device as gone) and reappears about
a second later; reopen it. If macOS ever picks a different suffix for the
composite device, look for `/dev/cu.usbmodem07B2D46E71C5*`. While the
composite device is up, the drive mounts as `OPENSCOPE` (read-only).

**Root cause status.** Not reproduced statically: the descriptor bytes, the
EP0 data stage and the packet memory all check out, and the host-visible
difference between the two devices is just the descriptors (and, before this
change, where they were stored: the composite tables were `const` in flash at
0x080B97F4, the library's in SRAM; they are now in SRAM too). One real
library defect was found on the way and fixed (`usb_conf.h`: the endpoint
register's CTR flags were cleared with a read-modify-write that could drop the
other direction's completion; see "CDC stall fixed" in the boot-logo section) but it
needs simultaneous IN and OUT traffic on one endpoint, which enumeration does
not have. The bench answer is `usb trace` after a FALLBACK.

**Device.** `src/drivers/usb_composite.c`: interfaces 0-1 are the CDC (one IAD,
the same endpoints 0x82 / 0x81 / 0x01, the library's unmodified `cdc_class.c`
reached through its handler table), interface 2 is MSC SCSI/bulk-only on
0x83 / 0x03 (class triple 08 06 50, as stock's). Device class EF/02/01 (IAD).
VID:PID stays **2E3C:5740** (the host scripts key "app running" on it);
`bcdDevice` is 2.01 instead of 2.00 so a host that caches descriptors per
VID/PID/revision (Windows) treats it as a new device. The bytes are
`src/util/usb_comp_desc.c` (pure C, `_Static_assert`ed against the library's
macros in `usb_composite.c`, validated by `make test-usb`). Packet memory:
EPT2's never-used RX slot went to EPT3, so the device fits the 512 bytes
exactly (`usb_conf.h`). `make USB_MSC=0` leaves the composite code out.

**Volume** (`src/util/gallery_vfat.c`): 512 B sectors, 4 KB clusters, 1 reserved
sector, 2 FATs of 1 sector, 32 root entries, 1024 sectors = 512 KB, 127 clusters
(FAT12 by count). Label `OPENSCOPE`. Gallery slot k owns clusters `2 + 10k ..
11 + 10k` whether or not it holds a shot, so a data sector maps to (slot, byte
offset) by division and a file never moves when another is deleted; the
38518-byte BMP is read straight from `slot + 256` (`gallery_read_bmp`), the
rest of its last cluster reads zero. Root entries are `<seq>.BMP` with the NT
lower-case flags, attribute read-only + archive, a fixed 2026-01-01 timestamp
(the slot header has no time). Hosts list `1.bmp`, `2.bmp`, ... A number above
99999999 does not fit 8.3 and is listed as `SHOT~<slot>.bmp` (it would take
10^8 saves). Nothing is written to the W25Q, ever: there is no write path.

**Transport** (`src/drivers/usb_msc.c`, SCSI logic in `src/util/msc_scsi.c`).
The USB interrupt decodes the CBW and answers everything that needs no flash
(INQUIRY, TEST UNIT READY, READ CAPACITY, MODE SENSE, REQUEST SENSE, READ
FORMAT CAPACITIES, refusals). A READ is latched and the `usb_dbg` task computes
it sector by sector (`usb_msc_poll`), because W25Q reads take `flash_fs`'s mutex,
which an ISR cannot; the ISR wakes the task with a task notification, so there
is no 10 ms poll per sector. RAM: one 512-byte buffer (also the CBW buffer and
the sink for dropped write data) plus ~150 bytes of state; measured on the
default build +328 B bss in all, since the library's 256-byte CDC string buffer
no longer links. Mismatched host/device data phases follow BOT 6.7 (zero fill
up to the host's length where a short packet cannot end the phase; dropped
data on host-to-device; phase error where the spec asks for one).

**Medium.** While the composite device is up the LUN has a medium (sharing on
inserts it). Off at boot like stock, and because a host warns "not ejected
properly" every time a shared unit is unplugged or reflashed. Not persisted
(stock's is not either). A **host eject** (START STOP UNIT, LoEj=1 Start=0, with
a zero POWER CONDITION field) only takes the medium out: the device stays
CDC + MSC (no re-enumeration, so the shell's port does not flap), `usb` says
`medium=out (host ejected ...)`, Settings shows "Ejected". The medium comes back
with `usb share on`, the Settings row's OK (on "Ejected" it re-inserts rather
than turning sharing off), or a host LOAD (START STOP UNIT LoEj=1 Start=1).
`usb share off` returns to CDC-only. Chosen over "eject turns sharing off":
that would re-enumerate under the user's terminal on a click in Finder.
Before attaching, the gallery is indexed (`usb_msc_prepare`), so the host's
first probe sees the final volume. `usb trace` lists, after the enumeration,
the first 64 SCSI commands of the attach (CDB, CSW status, sense; identical
repeats such as an absent-medium TUR poll fold into one line). Inserting it, and every gallery save or
delete while shared (`gallery_generation()`), raises UNIT ATTENTION / MEDIUM
MAY HAVE CHANGED once, so the host drops its cached FAT and re-reads: a shot
saved while the drive is mounted appears on the PC within the host's polling
interval. The index is never rebuilt in the middle of a READ.

**Not built: stock's volume `2:` as a second LUN.** It could be read without
any FAT code (pass-through of `uservol`), but: its BPB says 4096 bytes per
sector, so it would need a second logical block size and host support for
4 KB-sector FAT12 we have not verified; its FAT nominally spans `0xD00000+`
(our fwcache/user-cal/settings/scratch/cal backup), which a read-only view must
present as zeros, i.e. a volume whose FAT can point at clusters that read
back as something else; and our bench units' `2:` holds no screenshots (the
2026-06-13 flash map). If a user with stock screenshots needs them, the shell's
`flash dump` of `0x200000..` plus a host FAT tool reaches them today.

## Boot logo upload (built)

Stock reads `2:/LOGO/LOGO2C53T.jpg` from its own FAT. OpenScope stores its own
image instead (no FAT, no JPEG decoder, no RAM): `firmware/src/util/logo_store.h`.

- **Region**: `FLASH_REGION_LOGO`, 0xF70000..0xF9FFFF (192 KB), carved from the
  unused tail of `modules` (now 0xF20000, 320 KB). One record: 24-byte header at
  +0 (magic `OSL1`, version, format, 320x240, length, payload CRC32, header
  CRC32; written LAST as the commit record) and 153,600 bytes of RGB565
  little-endian at +256.
- **Upload** (`scripts/logo_put.py upload image.png [--port ...]`):
  `logo put 153600 <crc32>` erases the slot (header sector first; 2-15 s),
  then 150 blocks of `logo blk <off> <n> <crc32>` (header ends in `\r` only) ->
  `logo: ready` -> `n` <= 1024 raw bytes -> `logo: ok <received>` once the block
  is CRC-checked and programmed; `logo end` re-reads the payload from flash,
  compares the CRC and only then writes the header. A block whose reply is lost
  or bad is retried from `logo pos` (the device drops a half-received block
  after 2 s of silence); 8 failures in a row abort. A failed or interrupted
  upload leaves the slot empty and the built-in splash shows. Expected time:
  ~5-10 s in all (erase dominates). `--hex` keeps the old 48-byte
  `logo data <off> <hex>` lines for older firmware (minutes per upload).
  `scripts/test_logo_put.py` drives the retry logic through a model of the
  firmware with a lost ack, a corrupted block, a stalled block and a lost
  `ready`.
- **CDC stall fixed (2026-11-11)**: the first bench upload stopped at 12 % and
  the shell never answered again until a replug: the CDC IN completion had
  been lost (`g_tx_completed` stuck at 0). Cause: the AT32 library clears an
  endpoint register's TXTC/RXTC flags (rc_w0) with a read-modify-write that
  writes the *other* flag back as read, so an IN completion landing during the
  clear of an OUT completion on the same endpoint (EP1 carries both CDC bulk
  directions) is erased, and vice versa. The shell's per-character echo of a
  stream of `logo data` lines made that overlap frequent. Fixed three ways:
  `usb_conf.h` overrides `USB_CLEAR_TXTC`/`USB_CLEAR_RXTC` with ST's
  write-1-to-the-other-flag form (`make test-usb` checks usbd_int.c compiles
  them); the shell recovers a lost completion from the endpoint's hardware
  state (`usb`: `tx_heals`, `rx_heals`) and drops output at once while the
  host is not reading; and echo goes out one packet per received chunk
  instead of one IN transfer per character.
- **Shell**: `logo info` (state, CRC re-check), `logo show [ms]` (preview over
  the running UI), `logo clear`.
- **Boot**: after settings and the backlight PWM are up, `logo_ui_boot_show()`
  turns the backlight off, streams the image into the LCD, restores the stored
  brightness; the key task then holds ~1 s (any key skips) and releases the
  screen. No logo stored: nothing changes. RAM cost: 16 bytes of bss.
- **Not built**: reading a logo a user installed under stock
  (`2:/LOGO/LOGO2C53T.jpg`) needs a read-only FAT12 walker and a JPEG decoder.
