// OpenScope capture core for the FNIRSI 2C53T: the constants and types every module shares.
// This file is the one place where the SPI protocol's numbers live.
`default_nettype none
package core_pkg;
  // ---- opcodes (first byte of a frame)
  localparam logic [7:0] OP_TIMEBASE = 8'h01;   // 01 vv  timebase code
  localparam logic [7:0] OP_LIVE     = 8'h03;   // 03     byte 2 = converter A now, byte 4 = converter B now
  localparam logic [7:0] OP_REC_A    = 8'h04;   // 04     record of converter A
  localparam logic [7:0] OP_REC_B    = 8'h05;   // 05     record of converter B
  localparam logic [7:0] OP_SOURCE   = 8'h06;   // 06 vv  trigger source, bit 0: 0 = A, 1 = B
  localparam logic [7:0] OP_EDGE     = 8'h07;   // 07 vv  bits 1:0 edge mode, bits 7:4 hysteresis in codes
  localparam logic [7:0] OP_LEVEL    = 8'h08;   // 08 vv  trigger level, raw code
  // E0..EF exist only in OpenScope's core. Never send them to another design.
  localparam logic [7:0] OP_DBG_STAT = 8'hE0;   // E0     status block
  localparam logic [7:0] OP_DBG_T50  = 8'hE1;   // E1 vv  bench: 50 MS/s sample cycles
  localparam logic [7:0] OP_IDENT    = 8'hE2;   // E2     which core is this, and what can it do
  // (02 vv is accepted and has no effect; every other opcode is ignored.)
  // Rule for every reply: bit 7 of byte 1 is 0. That bit leaves before the opcode has been taken in.

  localparam logic [7:0] REPLY_FIRST = 8'h80;   // byte 0 of every reply
  localparam logic [7:0] DBG_HEAD    = 8'hB5;   // E0 block: first and last byte
  localparam logic [7:0] DBG_TAIL    = 8'h5B;
  localparam logic [7:0] TB_50MS     = 8'h05;   // the timebase code with the converters at 50 MHz

  // ---- identity (op E2): bytes 2..8 of the reply
  localparam logic [7:0] ID_MAGIC0   = 8'h4F;   // 'O'
  localparam logic [7:0] ID_MAGIC1   = 8'h53;   // 'S'
  localparam logic [7:0] PROTO_VER   = 8'h01;   // changes when a firmware written for the old one would misread
  // capability bits
  localparam logic [7:0] CAP_SEAM    = 8'h01;   // ops 04/05 carry the seam in bytes 1-2
  localparam logic [7:0] CAP_EDGE    = 8'h02;   // reg 07: trigger direction and hysteresis
  localparam logic [7:0] CAP_REAL50  = 8'h04;   // timebase 05 clocks the converters at a real 50 MHz
  localparam logic [7:0] CAP_LOCKRST = 8'h08;   // capture held until the PLL has locked; E0 byte 15 counts losses
  localparam logic [7:0] CAPS        = CAP_SEAM | CAP_EDGE | CAP_REAL50 | CAP_LOCKRST;

  // ---- record geometry
  localparam int ADDR_W = 10;                               // 1024 samples per record
  localparam int PRE_LAST  = 511;               // samples before the trigger: 512
  localparam int POST_LAST = 510;               // samples from the trigger on: 512

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
