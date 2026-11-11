# On-device screenshots, gallery, USB Sharing and boot logo (parity C16)

Status 2026-11-11: **screenshot save, 3x3 gallery and the `screenshot` shell
command are implemented** (host-tested; bench test pending). **USB Sharing (mass
storage) and the boot-logo upload are designed here but not built**: both need
RAM the warm-handoff images do not have (see the last two sections).

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
```

`<n>` may be `3` or `3.bmp`.

## USB Sharing (mass storage): design, not built

The AT32 library already ships `usbd_class/msc` and `composite_cdc_msc`, and
FatFs R0.14b, so this needs no new stack. Two things block it today:

1. **RAM.** `msc_bot_scsi.c` keeps a static `MSC_MAX_DATA_BUF_LEN` = 4096-byte
   buffer, and a virtual FAT needs one 4 KB sector staged at a time. The default
   image has ~2.5 KB of RAM left and the warm-handoff images ~0.5 KB. The way
   out is the shared pool: stock stops the scope in USB Sharing (state 4 gates
   acquisition at `0x08020EAC`), so in that mode the 100 KB pool is free. Patch
   the BOT layer to take its buffer from `shared_mem_acquire()` (a new owner
   `SHMEM_OWNER_USBMSC`) on entering the mode and release it on eject.
2. **Which bytes to expose.** Not stock's volume `2:` as is: its FAT spans
   `0xD00000+`, and a host that writes to the drive could allocate our clusters
   (parity2_static.md 4.5: never a writable view of `0xD00000+`). Two options:
   - **(a) A synthetic read-only FAT12** (recommended first step). The device
     answers READ(10) for a volume it computes per sector: boot sector, FAT and
     root directory generated from `gallery_scan()`, one file per valid slot
     (`<n>.BMP`, 38518 bytes, ten clusters each, mapped one-to-one onto the slot's
     BMP bytes at `slot+256`). WRITE(10) fails with DATA PROTECT; INQUIRY reports
     write-protected. No FAT on the W25Q is ever mounted or written, nothing at
     `0xD00000+` can be reached, and the host sees exactly the gallery. Tests:
     the generated BPB/FAT/directory parse with `fsck.fat -n` on a host image;
     sector-to-slot mapping; WRITE refusal.
   - **(b) Read-through of stock's `2:`** for users who keep stock screenshots:
     LUN capacity = volume `2:`, reads below `0xD00000` pass through, reads at
     or above return zeros, all writes refused. Needs no FAT code at all but
     shows only stock's files, not ours.
3. **Mode switching.** Stock exposes MSC only; entering USB Sharing stops CDC.
   Ours: Settings item "USB Sharing" and shell `usb share` re-enumerate as
   composite CDC+MSC (so the shell keeps working, interface association
   descriptors from `cdc_msc_desc.c`), or MSC-only behind a build flag if a host
   misbehaves; leave by MENU or host eject (START STOP UNIT), then re-enumerate
   as CDC. The descriptor and LUN count of stock's own mode can be read on the
   host with `lsusb -v` while a stock unit is in USB Sharing (no SWD).

Estimated size: L (BOT buffer patch, virtual FAT generator + tests, enumeration
switch, settings item).

## Boot logo upload: design, not built

Stock reads `2:/LOGO/LOGO2C53T.jpg` from its own FAT. For OpenScope:

1. **Upload**: `logo put <len> <crc32>` over CDC into a dedicated slot (reuse one
   40 KB region slot pattern: a 64 KB area carved from `modules`, header + JPEG,
   CRC-checked, same commit-last rule as the gallery). No FAT involved.
2. **Optional stock compatibility**: a read-only FAT12 reader that finds
   `2:/LOGO/LOGO2C53T.jpg` on stock's volume (path walk + cluster chain, reads
   only, never mounts read-write, never formats), so a logo a user installed
   under stock keeps working.
3. **Decode**: TJpgDec (BSD-style licence, baseline JPEG, ~3.1 KB work area) at
   boot, before the scheduler starts, with the work area in the shared pool
   (free at boot). Draw with the backlight at 0, then raise it to the stored
   brightness, as stock does (`0x0802C04A..0x0802C06C`). Any failure keeps the
   built-in splash. Shell `logo info` reports source/size/decode result.

Estimated size: M.
