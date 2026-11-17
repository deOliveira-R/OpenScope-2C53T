// OpenScope capture core for the FNIRSI 2C53T: the constants and types every module shares.
// This file is the one place where the SPI protocol's numbers live.
`default_nettype none
package core_pkg;
  // ---- opcodes (first byte of a frame)
  localparam logic [7:0] OP_TIMEBASE = 8'h01;   // 01 vv  timebase code
  localparam logic [7:0] OP_LEGACY02 = 8'h02;   // 02 vv  accepted, no effect (the firmware's arm burst sends 02 03)
  localparam logic [7:0] OP_LIVE     = 8'h03;   // 03     byte 2 = converter A now, byte 4 = converter B now
  localparam logic [7:0] OP_REC_A    = 8'h04;   // 04     record of converter A
  localparam logic [7:0] OP_REC_B    = 8'h05;   // 05     record of converter B
  localparam logic [7:0] OP_SOURCE   = 8'h06;   // 06 vv  trigger source, bit 0: 0 = A, 1 = B
  localparam logic [7:0] OP_EDGE     = 8'h07;   // 07 vv  bits 1:0 edge mode, bits 7:4 hysteresis in codes
  localparam logic [7:0] OP_LEVEL    = 8'h08;   // 08 vv  trigger level, raw code
  localparam logic [7:0] OP_FORCE    = 8'h09;   // 09     hold a record without a trigger (see capture.sv); not a register
  // E0..EF exist only in OpenScope's core. Never send them to another design.
  localparam logic [7:0] OP_DBG_STAT = 8'hE0;   // E0     status block
  localparam logic [7:0] OP_DBG_T50  = 8'hE1;   // E1 vv  bench: 50 MS/s sample cycles
  localparam logic [7:0] OP_IDENT    = 8'hE2;   // E2     which core is this, and what can it do
  // (Every other opcode is ignored.)
  // Rule for every reply: bit 7 of byte 1 is 0. That bit leaves before the opcode has been taken in.

  localparam logic [7:0] REPLY_FIRST = 8'h80;   // byte 0 of every reply
  localparam logic [7:0] DBG_HEAD    = 8'hB5;   // E0 block: first and last byte
  localparam logic [7:0] DBG_TAIL    = 8'h5B;
  localparam logic [7:0] TB_50MS     = 8'h05;   // the timebase code with the converters at 50 MHz

  // ---- reg 07 (OP_EDGE): value = (hysteresis << EDGE_HYS_SHIFT) | edge mode
  localparam logic [7:0] EDGE_EITHER   = 8'h00;   // either direction (what the arm burst writes)
  localparam logic [7:0] EDGE_RISING   = 8'h02;   // rising only
  localparam logic [7:0] EDGE_FALLING  = 8'h03;   // falling only
  localparam logic [7:0] EDGE_MODE_MASK = 8'h03;
  localparam int EDGE_HYS_SHIFT = 4;              // hysteresis in codes, bits 7:4

  // ---- ops 04 / 05: bytes 1-2 of the reply (the seam), then the samples
  localparam logic [7:0] SEAM_VALID    = 8'h04;   // byte 1 bit 2: bytes 1-2 hold the seam of a held record
  localparam logic [7:0] SEAM_HI_MASK  = 8'h03;   // byte 1 bits 1:0 = seam bits 9:8; byte 2 = seam bits 7:0
  localparam logic [7:0] SEAM_FORCED   = 8'h08;   // byte 1 bit 3, with SEAM_VALID: the record was held by op 09, not by a trigger
  localparam logic [7:0] SEAM_B1_MASK  = 8'hF4;   // byte 1 & this == SEAM_VALID, else no seam is given
  localparam int REC_FIRST_SAMPLE = 3;            // reply byte of sample 0
  localparam int LIVE_BYTE_A = 2;                 // op 03: reply byte of converter A
  localparam int LIVE_BYTE_B = 4;                 // op 03: reply byte of converter B

  // ---- op E0: reply bytes of the status block
  localparam int DBG_I_HEAD = 3;                  // B5
  localparam int DBG_I_TAG = 4;
  localparam int DBG_I_STATE = 5;                 // lock, pair held, edge[1:0], 0, forced, capture state
  localparam int DBG_I_TB = 6;
  localparam int DBG_I_SRC = 7;
  localparam int DBG_I_LVL = 8;
  localparam int DBG_I_WP_HI = 9;
  localparam int DBG_I_WP_LO = 10;
  localparam int DBG_I_NHELD = 11;                // records held so far
  localparam int DBG_I_TAIL = 12;                 // 5B
  localparam int DBG_I_T50 = 13;
  localparam int DBG_I_EDGE = 14;                 // {hysteresis, 00, edge}
  localparam int DBG_I_NLOST = 15;                // times the PLL lost lock (repeats to the end of the frame)
  localparam logic [7:0] DBG_STATE_LOCK = 8'h80;  // state byte: PLL locked
  localparam logic [7:0] DBG_STATE_PAIR = 8'h40;  // state byte: this frame belongs to a held pair
  localparam logic [7:0] DBG_STATE_FORCED = 8'h04;   // state byte: the held record was forced (op 09)
  localparam logic [7:0] DBG_STATE_CAP_MASK = 8'h03;   // state byte: capture state (ST_*)

  // ---- identity (op E2): bytes 2..8 of the reply
  localparam logic [7:0] ID_MAGIC0   = 8'h4F;   // 'O'
  localparam logic [7:0] ID_MAGIC1   = 8'h53;   // 'S'
  localparam logic [7:0] PROTO_VER   = 8'h01;   // changes when a firmware written for the old one would misread
  // capability bits
  localparam logic [7:0] CAP_SEAM    = 8'h01;   // ops 04/05 carry the seam in bytes 1-2
  localparam logic [7:0] CAP_EDGE    = 8'h02;   // reg 07: trigger direction and hysteresis
  localparam logic [7:0] CAP_REAL50  = 8'h04;   // timebase 05 clocks the converters at a real 50 MHz
  localparam logic [7:0] CAP_LOCKRST = 8'h08;   // capture held until the PLL has locked; E0 byte 15 counts losses
  localparam logic [7:0] CAP_FORCE   = 8'h10;   // op 09 holds a record without a trigger; byte 1 bit 3 of ops 04/05 marks it
  localparam logic [7:0] CAPS        = CAP_SEAM | CAP_EDGE | CAP_REAL50 | CAP_LOCKRST | CAP_FORCE;

  // ---- clock and record geometry
  localparam int CLK_HZ = 125000000;             // the core clock: 25 MHz reference x 5
  localparam int RATE50_HZ = 50000000;           // sample rate at TB_50MS (converters clocked at 50 MHz)
  localparam int TB_CODES = 21;                  // timebase codes 0x00..0x14; larger codes act as 0x14
  localparam int ADDR_W = 10;                               // 1024 samples per record
  localparam int PRE_LAST  = 511;               // samples before the trigger: 512
  localparam int POST_LAST = 510;               // samples from the trigger on: 512
  localparam int TRIG_INDEX = 512;               // a held record's trigger crossing, counted from the seam

  // ---- capture state, as the E0 block shows it
  typedef enum logic [1:0] {
    ST_PRE   = 2'd0,    // filling the part before the trigger
    ST_ARMED = 2'd1,    // waiting for the trigger
    ST_POST  = 2'd2,    // filling the part after it
    ST_HELD  = 2'd3     // record frozen until it has been read
  } cap_state_e;

  // ---- the registers the firmware writes
  typedef struct packed {
    logic [7:0] tb;          // timebase code
    logic [7:0] src;         // trigger source (bit 0)
    logic [7:0] lvl;         // trigger level
    logic [1:0] edge_mode;   // 00 either direction, 10 rising only, 11 falling only
    logic [3:0] hys;         // hysteresis of the one-direction modes, in codes
    logic [2:0] t50a;        // 50 MS/s: sample cycle of converter A (debug)
    logic [2:0] t50b;        // 50 MS/s: sample cycle of converter B (debug)
  } cfg_t;
  localparam logic [7:0] RST_TB  = 8'h08;
  localparam logic [7:0] RST_SRC = 8'h00;
  localparam logic [7:0] RST_LVL = 8'h80;

  // timebase code -> how many 125 MHz cycles per stored sample
  function automatic logic [17:0] tb_divider(input logic [7:0] tb);
    case (tb)
      8'h00, 8'h01, 8'h02, 8'h03, 8'h04: tb_divider = 18'd1;
      8'h05: tb_divider = 18'd2;        // 50 MS/s: not used, the samples follow the converter clock
      8'h06: tb_divider = 18'd5;       8'h07: tb_divider = 18'd10;
      8'h08: tb_divider = 18'd25;      8'h09: tb_divider = 18'd50;
      8'h0A: tb_divider = 18'd100;     8'h0B: tb_divider = 18'd250;
      8'h0C: tb_divider = 18'd500;     8'h0D: tb_divider = 18'd1000;
      8'h0E: tb_divider = 18'd2500;    8'h0F: tb_divider = 18'd5000;
      8'h10: tb_divider = 18'd10000;   8'h11: tb_divider = 18'd25000;
      8'h12: tb_divider = 18'd50000;   8'h13: tb_divider = 18'd100000;
      default: tb_divider = 18'd250000; // 0x14 and the roll codes: 500 S/s
    endcase
  endfunction
endpackage
`default_nettype wire
