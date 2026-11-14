# Binary stream protocol (FEATURE_STREAM)

Status: prototype, protocol 1.0. Firmware: `firmware/src/stream/` (bench image `app-coldtrace-meter`).
Host: `scripts/scope_stream.py`. Encoder test and cross-check vectors: `firmware/tests/test_stream.c`,
`firmware/tests/fixtures/stream/`.

The scope is a measuring device that happens to have a screen and keys. The stream lets a host show what the
instrument measures without asking the screen: captures, meter readings and the full property state go out
as binary frames on the **same CDC port as the text shell**. The host demultiplexes the two. Configuration
stays on the direct route (`get` / `set` text commands, answered while the stream runs).

## 1. Starting and stopping

```text
stream on [scope] [meter] [props] [fast]   no names = all three; `fast` = spin-paced packets (section 7)
stream off                                 the counters as text; a last STATS frame follows the prompt
stream status                              two lines: state, then sent/dropped per type
```

`stream on` prints its reply and the prompt first; frames follow. The stream also stops by itself when USB is
unplugged (`off_reason=usb disconnected`) or when every frame it tried for 5 s failed (`off_reason=host not
reading`), so a terminal opened later sees the shell, not binary. A long command or a firmware load is not a
failure: only failed attempts start that clock. It is never on after boot.

Not a property group: see section 8.

## 2. Interleaving rule

The shell task sends the frames itself, from its loop, **between commands**: after it has handled the bytes
it received and before it idles. Consequences:

- A command's echo, reply and prompt are contiguous on the wire if the host sends the command line in one
  write. No frame lands inside a command's output, and none inside a line the shell prints.
- A long command delays frames; records published meanwhile are counted as `missed` (section 5).
- A person typing one character at a time may see a frame between two echoed characters; the demultiplexed
  text is still exactly the shell's bytes, in order.
- No frames while the firmware loader is receiving, while it drains an aborted image, or during a raw
  `logo blk` upload.
- Frames are also mirrored to RTT, as all shell output is. With an RTT reader attached, `rtt_write` can
  wait for it before each packet, outside the stream's own time limits: measure with no RTT reader.

## 3. Frame

All fields little-endian; floats IEEE-754 single.

| off | size | field | |
|---|---|---|---|
| 0 | 1 | `0xFB` | marker; 0xFB never occurs in UTF-8 text |
| 1 | 1 | `0x53` (`S`) | marker |
| 2 | 1 | type | section 4 |
| 3 | 1 | flags | 0 from the device; a broker sets bit0 REPLAY (a cached copy for a late joiner: not part of the live sequence) and bit1 FILTERED (this client's stream leaves some types out: seq gaps are expected) |
| 4 | 2 | seq | +1 for every frame begun, all types, wraps at 65536 |
| 6 | 2 | len | payload bytes, at most 4096 |
| 8 | 2 | hcrc | CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over bytes 0..7 |
| 10 | len | payload | |
| 10+len | 4 | crc | CRC-32 (IEEE, as zlib) over bytes 2 .. 9+len |
| | (1) | `0x00` | only if 14+len is a multiple of 64 |

The pad byte keeps every frame ending on a short USB packet, so no host driver holds the last full packet
back. **Outside frames, a host drops NUL bytes** (the shell never prints NUL).

### Demultiplexing (what `scope_stream.py demux()` does)

1. Bytes before a `0xFB` are text.
2. At `0xFB`: wait for 10 bytes. If byte 1 is not `0x53`, hcrc does not match or len > 4096, the `0xFB` is
   text; continue at the next byte.
3. Wait for the whole frame. If the CRC-32 fails, count a CRC error, drop the 10 header bytes, and scan on
   **from the byte after the header** (never skip by a length that failed its CRC). This finds a frame that
   follows a truncated one, and keeps shell text that followed it. The rest of the broken payload comes out as
   text.
4. A frame of a type the host does not know is skipped by its length.

Binary command replies (`screenshot get`, `flash dump`) can contain the marker. The header CRC makes a false
start unlikely (1 in 65536 per `0xFB 0x53`), and step 3 recovers from one. Do not run them while
streaming if the host needs them byte-exact.

### Compatibility

- Type codes are never reused. A new kind of data is a new type.
- Payloads only grow **at the end**. A host reads the fields it knows and ignores the rest.
- The major version (HELLO) changes only if a field changes meaning; a host refuses a major it does not know.

## 4. Types

| code | name | sent |
|---|---|---|
| 0x01 | HELLO | first, after every `stream on` |
| 0x06 | PROPDESC | after HELLO, one per property (with `props`) |
| 0x02 | PROPS | after the PROPDESCs, then whenever a property value or group epoch changes (checked every 50 ms) |
| 0x03 | RECORD | each newly published scope record, once (with `scope`) |
| 0x04 | METER | each new meter reading (with `meter`) |
| 0x05 | STATS | every second, and on `stream off` |
| 0x07 | MEAS | reserved: measurements (Vpp, Freq, ... as the badges compute them) |
| 0x08 | GEN | reserved: generator read-back |
| 0x09 | CANFRAME | reserved: decoded CAN frames |
| 0x0A | ROLL | reserved: roll points (a batch of CH1/CH2 point pairs per frame) |

Strings are `u8 length` + bytes (no NUL).

### HELLO (0x01)

| field | type | |
|---|---|---|
| proto_major, proto_minor | u8, u8 | 1, 0 |
| caps | u8 | what this firmware can send: bit0 scope, bit1 meter, bit2 props |
| on | u8 | what this `stream on` asked for (same bits) |
| max_payload | u16 | 4096 |
| rec_samples | u16 | samples per channel in a record (1024) |
| n_props | u16 | properties in the table |
| table_crc | u32 | CRC-32 over (id u16, type, unit, decimals, group) of every property, table order |
| fw | str | firmware build text |

A host that caches PROPDESC keys it by `table_crc`. The host resets its sequence-gap and generation checks at
HELLO.

### PROPDESC (0x06)

`id u16, type u8 (0 bool, 1 enum, 2 int, 3 mask), unit u8, decimals u8, group u8, flags u16, min i32,
max i32, def i32, name str, unit_sym str`. The values are those of `src/model/model_table.h`; `list` and
`describe` print the same in text. A value v of an int property with d decimals reads as v / 10^d unit_sym.

### PROPS (0x02)

`table_crc u32, n_groups u8, epoch u32 x n_groups, n_props u16, then n_props x (id u16, value i32)`,
in table order (62 properties: 441 bytes on the wire).

The trigger is a hash over all values and epochs, not the epochs alone: today the keys still write most
settings directly (only `set` and the mode change go through the model's executor), so a V/div key press
moves no epoch. The hash sees every writer.

### RECORD (0x03)

| field | type | |
|---|---|---|
| gen | u32 | acquisition frame generation of this record (even; +2 per published record) |
| t_ms | u32 | device milliseconds when the sender took it |
| fs_hz | f32 | sample rate of this record (2x when interleaved) |
| s_per_div | f32 | the screen's time/div (Y-T timebase, or roll) |
| n | u16 | samples per channel (1024) |
| trig_idx | u16 | sample index of the hardware trigger, 0xFFFF = not known (the record is not time-ordered). **Not** "untriggered": whether the record was triggered is `kind` |
| flags | u8 | bit0 interleaved, bit1 time-ordered, bit2 roll, bit3 coherent (CH1 and CH2 one moment), bit4 running, bit5 true scale |
| kind | u8 | trig_acq_t: 1 triggered (handed over), 2 free-run, 3 free-run split |
| tb_code | u8 | timebase code selected (tb.code) |
| nch | u8 | channel entries that follow (1 or 2: the enabled channels) |
| per channel (16 B) | | `ch u8 (0 = CH1), vdiv_idx u8, zero_code u8, coupling u8, position i16, probe_x u16, volts_per_count f32, volts_per_div f32` |
| samples | u8 x n x nch | record codes, channel order as the table |
| gen_end | u32 | the generation read again **after** the samples |

A triggered record (`kind` 1) is time-ordered only when the acquisition finds the seam of the FPGA's write
pointer (`fpga_acq_unrotate_staging`). For a fast periodic signal (many periods per record) there is no seam
to find, so `trig_idx` is 0xFFFF while the record is triggered; the screen then aligns it with its soft
trigger (the signal's midline plus `trig.level`, edge `trig.edge`, on `trig.source`, past the first 128
samples). `scope_stream.py` does the same (`record_reference`).

Volts: `(code - zero_code) * volts_per_count`, at the probe tip (the probe factor is in). `volts_per_count`
0 means the range is uncalibrated: show codes. Seconds: `(i - trig_idx) / fs_hz` (the record centre when
there is no trigger index). The screen draws 12 divisions of `s_per_div` and 8 of `volts_per_div`, with the
centre line at code 100 (true scale).

**Torn**: `gen_end != gen` means a new record was committed while this one was being sent, so the samples may
mix two captures. Discard or mark it. **Missed**: a step in `gen` larger than 2 means records were published
that the host never got (sent or dropped, the firmware counts which).

2 channels: 2108-byte payload, 2122 bytes on the wire (34 packets). 1 channel: 1068 / 1082 (17 packets).

### METER (0x04)

`update_count u32, t_ms u32, value f32, bcd i32, func u8 (meter.func), auto_fn u8, result_class u8,
status_code u8, decimal_pos u8, range_index u8, flags u8 (bit0 valid, bit1 negative, bit2 AC, bit3 auto
range, bit4 hold, bit5 continuity beep), unit str, display str, range_label str`. A frame is sent when
`update_count` moves, so only while the meter chip is delivering readings.

### STATS (0x05)

`t_ms u32, bytes u32, rec_missed u32, rec_torn u32, aborts u32, n u8, then n x (type u8, sent u32,
dropped u32)` for types 1..n. `bytes` = stream bytes handed to the CDC since `stream on`; `aborts` = frames
abandoned mid-way; `dropped` = frames not started because the CDC was busy, plus aborted ones;
`rec_missed` = records published and superseded before the sender looked (the shell was busy with a
command, or a send was in progress).

## 5. Sender rules (firmware)

- **Exactly once.** A record is identified by its generation. The sender takes a new even generation, reads
  the metadata, re-reads the generation (a commit during the metadata: try next pass), marks it taken, and only
  then decides to send or drop. It never sends the same generation twice and never waits for one.
- **Drop, don't wait.** A frame starts only if the CDC IN endpoint is free within 2 ms; otherwise it is dropped
  and counted. Within a frame, one packet that cannot leave within 50 ms abandons the frame (`aborts`); the
  host resynchronises on the next marker. A host that stopped reading costs at most one 50 ms stall per
  frame (with no RTT reader attached).
- **No buffers.** Samples go from the published record buffers through one 64-byte packet on the stack. The
  CRC is taken over that copy, so it always matches what was sent; tearing is reported by `gen_end`, not by a
  CRC error.
- Priority within one shell-loop pass (one frame per pass): HELLO, PROPDESC, PROPS, RECORD, METER, STATS.
  After a frame the loop polls RX at once and does not idle, so a backlog drains while commands are still
  read between frames.

## 6. Adding a type

Pick the next free code (never a reserved one for another purpose), append the payload description here, add
the encoder to `stream_frame.c` with a case in `tests/test_stream.c` and the vectors, and the decoder to
`scope_stream.py decode()`. Old hosts skip it by length.

## 7. Rates (estimated from the code; to be measured)

- The write path (`usb_send_bytes`) sends one 64-byte packet and, before the next, sleeps one tick (1 ms)
  until the IN completion arrives. Paced by the tick that is **one packet per millisecond, about 64 KB/s**,
  the same as every shell reply.
- RECORD, 2 channels: 34 packets, about 34 ms of the shell task per record; the ceiling is about 29
  records/s. At the Y-T cycle (about 4.6 records/s) the stream uses about 10 KB/s, 16 % of the channel time.
- `fast` polls the completion for about 0.3-0.5 ms before falling back to the tick. The bus itself allows
  about 19 bulk packets per 1 ms frame (about 1.2 MB/s) if the host polls that often; how fast the AT32's
  IN completion and the host's controller turn around is not known without hardware. With `fast` the
  shell task does not yield while a frame goes out, which holds off the display (prio 1) and the prio-2
  tasks for the length of one frame.
- No lock is taken. The exposure window is the send time of one record (about 34 ms tick-paced, 17 ms for
  one channel). The sender sees a new record within 10 ms of its commit (the loop's idle period, none
  while it is busy sending), so a record torn means the next commit came within about 35-45 ms.
  The commit cadence depends on the trigger state (fpga.c, EXP-72): a triggering input gives handed-over
  records only (about 3.4-4.6/s): no tears expected, and the sender keeps up. **AUTO with no trigger**
  (`fpga autolive on`, the default) commits every roll read, about 33/s (every ~30 ms poll): a
  tick-paced 2-channel record cannot keep up (missed records) and most of those sent will be torn. Use
  `fast`, one channel, or a triggering input. Which of these holds on the bench is to be measured.
- Roll (reserved type 0x0A): the fastest roll step is 2 ms per point, 500 points/s, 1 KB/s for both channels.
  Bytes are not the limit; frames are. One frame per point would spend 500 of the 1000 packets/s. A batch per
  shell-loop pass (about every 10 ms, 5 point pairs, one packet) costs about 100 packets/s. The 301-point
  FIFO gives the sender about 0.6 s before points are lost.

## 8. Why there is no `stream.*` property group

The stream is a setting of the USB session, not instrument state: it is not persisted, not shown on the
screen, and it must be off at boot. As properties it would need a new group in `model.h` (`PG_COUNT`, the
epoch array, the per-group tests) and rows in `model_table.h`, all under `#if FEATURE_STREAM`, so the table,
`table_crc` and the persistence tests would differ per build, and its setter would run on the input task
while the sender state belongs to the shell task. It would also appear in the PROPS snapshot it sends. The
model has no notion of an optional group yet; when it gets one (a build-profile flag that keeps ids stable and
hides rows), `stream.*` can move there.

## 9. Sharing the scope: `scripts/scope_broker.py`

A serial port serves one program. The broker owns it and listens on 127.0.0.1 (default port 7321). Each
client connection carries the same mix as the port, shell text and frames, so a tool connects with
`socket://127.0.0.1:7321` where it used the serial device. Client side:

- shell lines are queued and run one at a time; each reply (echo, output, `> `) goes only to its sender,
  in order. A reply ends at `\n> ` followed by 30 ms of quiet; no prompt within `--cmd-timeout` (10 s)
  gives `broker: TIMEOUT ...`, and anything printed later goes to every client;
- `stream on [scope] [meter] [props] [fast]` and `stream off` are subscriptions, reference-counted: the device
  stream runs while anyone subscribes. Late joiners get HELLO, PROPDESC and PROPS from the cache (flag
  REPLAY) at once. A client that asked for fewer types gets its frames flagged FILTERED;
- `!broker status | release <seconds> | quit`; `release` stops the stream, closes the port for that long
  (for a flashing tool), then reopens and restores the subscriptions;
- unsolicited text (boot banner, late output) and broker notices (`broker: ...`) go to every client. The
  device disappearing is announced, the port is reopened every 0.5 s, and the stream is restored.
