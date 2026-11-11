# On-device screenshots, gallery, USB Sharing and boot logo (parity C16)

Status 2026-11-11: **screenshot save, 3x3 gallery and the `screenshot` shell
command are implemented** (host-tested; bench test pending). **USB Sharing is
implemented** as a read-only mass-storage drive synthesised from the gallery
(host-tested, macOS mounts the generated image; bench test pending, see the
USB Sharing section). The **boot-logo upload is built** (last section; bench test pending).

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
| scope, signal generator, settings | SAVE | save the screen; popup "Saved N.bmp" / "Gallery full: delete" / "Save failed" (the meter keeps SAVE = debug overlay) |
| scope | long SAVE | open the gallery (3x3, newest first) |
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
usb [status]             USB conf=1 msc=1 share=off files=.. blocks=1024x512 state=0 cmds=.. reads=.. sectors=..
                           ioerr=0 badcbw=0 resets=0 wrref=0 rescans=.. last_op=.. last_lba=..
usb share [on|off]       usb: share on
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
| CDC shell | gone while sharing | keeps working: the device is always CDC + MSC |
| enter / leave | re-enumeration | medium inserted / ejected, no re-enumeration |
| scope while sharing | stopped | keeps running |

**Device.** `src/drivers/usb_composite.c`: interfaces 0-1 are the CDC (one IAD,
the same endpoints 0x82 / 0x81 / 0x01, the library's unmodified `cdc_class.c`
reached through its handler table), interface 2 is MSC SCSI/bulk-only on
0x83 / 0x03 (class triple 08 06 50, as stock's). Device class EF/02/01 (IAD).
VID:PID stays **2E3C:5740** (the host scripts key "app running" on it);
`bcdDevice` is 2.01 instead of 2.00 so a host that caches descriptors per
VID/PID/revision (Windows) treats it as a new device. The serial string is the
library's MCU-UID algorithm, so macOS should keep naming the port
`/dev/cu.usbmodem<serial>1` (CDC comm is still interface 0); confirm on the
bench. Packet memory: EPT2's never-used RX slot went to EPT3, so the device
fits the 512 bytes exactly (`usb_conf.h`). `make USB_MSC=0` builds the CDC-only
device of before.

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

**Sharing on/off** = medium present or not (removable LUN): Settings "USB
Sharing" (the last row, On/Off) or `usb share on|off`. Off at boot like stock,
and because a host warns "not ejected properly" every time a shared unit is
unplugged or reflashed. Not persisted (stock's is not either). A host eject
(START STOP UNIT) turns sharing off. Turning it on, and every gallery save or
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
- **Upload** (`scripts/logo_put.py upload image.png`): `logo put 153600 <crc32>`
  erases the slot (header sector first), `logo data <off> <hex>` sends 48 bytes
  per line at strictly sequential offsets, `logo end` re-reads the payload from
  flash, compares the CRC and only then writes the header. A failed or
  interrupted upload leaves the slot empty and the built-in splash shows.
- **Shell**: `logo info` (state, CRC re-check), `logo show [ms]` (preview over
  the running UI), `logo clear`.
- **Boot**: after settings and the backlight PWM are up, `logo_ui_boot_show()`
  turns the backlight off, streams the image into the LCD, restores the stored
  brightness; the key task then holds ~1 s (any key skips) and releases the
  screen. No logo stored: nothing changes. RAM cost: 16 bytes of bss.
- **Not built**: reading a logo a user installed under stock
  (`2:/LOGO/LOGO2C53T.jpg`) needs a read-only FAT12 walker and a JPEG decoder.
