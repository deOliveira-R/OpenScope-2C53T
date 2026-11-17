// The commands (SCLK side): the registers the firmware writes and the bytes it reads back.
//
//   01 / 06 / 07 / 08 vv   write a register (see core_pkg). Each write drops the capture in flight.
//   04 / 05   record of converter A / B: byte 0 = 80, bytes 1-2 = the seam, then 1024 samples from memory
//             address 0. The seam: byte 1 = 00000 V s9 s8, byte 2 = s7..s0. V = 1: this frame reads a held
//             record whose oldest sample is at index s; the trigger crossing is at (s + 512) mod 1024. V = 0: no
//             pointer is given (no held record, or the record was held only after this frame had begun: V and s
//             come from one snapshot taken before the frame). The op 05 of a pair always has V = 1.
//             An op 04 that finds a held record starts a "pair": the record stays frozen until the op 05 frame
//             that follows has ended.
//   03        byte 2 = converter A now, byte 4 = converter B now.
//   E0        debug: B5, TAG, state (lock, pair held, edge[1:0], 00, capture state), timebase, source, level,
//             write pointer (2), records held so far, 5B, 50 MS/s cycles, {hysteresis, 00, edge},
//             times the PLL lost lock (this byte repeats to the end of the frame).
//   E1 vv     bench: 50 MS/s sample cycles, bits 2:0 for converter A and 6:4 for B (0..4 each).
//   E2        identity: bytes 2.. = 'O', 'S', protocol version, TAG, capability bits, log2 of the record length,
//             then the XOR of those six bytes (a check of the link as well: the content never changes).
`default_nettype none
module cmd #(
  parameter logic [7:0] TAG  = 8'hD4,
  parameter logic [2:0] T50A = 3'd0,
  parameter logic [2:0] T50B = 3'd0
) (
  input  wire                          sclk,
  input  wire                          cs_n,
  // from the SPI slave
  input  wire  [2:0]                   bit_i,
  input  wire  [3:0]                   byte_i,
  input  wire  [7:0]                   rx_byte,
  input  wire  [3:0]                   tx_byte_i,
  output logic [7:0]                   tx_byte,
  // record memory, read side
  output logic [core_pkg::ADDR_W-1:0]  rd_addr,
  input  wire  [15:0]                  rd_data,     // {B, A}
  // to the 125 MHz side
  output core_pkg::cfg_t               regs,
  output logic                         cfg_tog,     // toggles with every register write
  output logic                         rd04_tog,    // toggles when an op 04 starts a pair
  output logic                         pair_op05,   // this frame is the op 05 of a pair (a flop: it crosses)
  // from the 125 MHz side
  input  wire                          held,        // live (synchronised here)
  input  wire                          lock,
  input  wire  [7:0]                   nheld,
  input  wire  [7:0]                   nlost,
  input  wire  [core_pkg::ADDR_W-1:0]  snap_wp,     // snapshot taken before the frame: still while it runs
  input  wire  core_pkg::cap_state_e   snap_st,
  input  wire  [7:0]                   snap_a,
  input  wire  [7:0]                   snap_b
);
  logic [7:0] op = 8'd0;
  logic       pair_held = 1'b0;
  logic       h1 = 1'b0, h2 = 1'b0;
  initial begin
    regs = {core_pkg::RST_TB, core_pkg::RST_SRC, core_pkg::RST_LVL, 2'b00, 4'd0, T50A, T50B};
    cfg_tog = 1'b0; rd04_tog = 1'b0; rd_addr = '0; pair_op05 = 1'b0;
  end

  // ---- opcode and register writes
  always_ff @(posedge sclk) begin
    h1 <= held; h2 <= h1;
    if (!cs_n && byte_i == 4'd0 && bit_i == 3'd7) begin
      op <= rx_byte;
      pair_op05 <= (rx_byte == core_pkg::OP_REC_B) && pair_held;   // an op 04 changes pair_held and clears this
      if (rx_byte == core_pkg::OP_REC_A) begin pair_held <= h2; if (h2) rd04_tog <= ~rd04_tog; end
    end
    if (!cs_n && byte_i == 4'd1 && bit_i == 3'd7) begin
      if (op == core_pkg::OP_TIMEBASE) begin regs.tb  <= rx_byte; cfg_tog <= ~cfg_tog; end
      if (op == core_pkg::OP_SOURCE)   begin regs.src <= rx_byte; cfg_tog <= ~cfg_tog; end
      if (op == core_pkg::OP_EDGE)     begin regs.edge_mode <= rx_byte[1:0]; regs.hys <= rx_byte[7:4]; cfg_tog <= ~cfg_tog; end
      if (op == core_pkg::OP_DBG_T50)  begin regs.t50a <= rx_byte[2:0]; regs.t50b <= rx_byte[6:4]; cfg_tog <= ~cfg_tog; end
      if (op == core_pkg::OP_LEVEL)    begin regs.lvl <= rx_byte; cfg_tog <= ~cfg_tog; end
    end
  end

  // ---- record read-out. rd_addr = sample index of the NEXT byte: 0 while bytes 0..2 go out, then one more per byte.
  logic [15:0] rd_q = 16'd0;
  always_ff @(posedge sclk or posedge cs_n)
    if (cs_n) rd_addr <= '0;
    else if (bit_i == 3'd7 && byte_i >= 4'd2) rd_addr <= rd_addr + 1'b1;
  always_ff @(posedge sclk) if (bit_i == 3'd6) rd_q <= rd_data;         // one edge before the next byte starts

  // ---- reply
  wire [7:0] state  = {lock, pair_held, regs.edge_mode, 2'b00, snap_st};
  // The seam is valid only when the snapshot taken before this frame already showed the record held (snap_wp and
  // snap_st are one snapshot) and the frame is part of a pair. Bit 7 of byte 1 goes out before pair_held of an
  // op 04 is known, so it is a constant 0.
  wire       seam_v = pair_held && (snap_st == core_pkg::ST_HELD);
  wire       is_rec = (op == core_pkg::OP_REC_A) || (op == core_pkg::OP_REC_B);
  localparam logic [7:0] ID_DEPTH = core_pkg::ADDR_W;
  localparam logic [7:0] ID_CHECK = core_pkg::ID_MAGIC0 ^ core_pkg::ID_MAGIC1 ^ core_pkg::PROTO_VER ^ TAG ^ core_pkg::CAPS ^ ID_DEPTH;
  always_comb begin
    tx_byte = 8'h00;
    if (tx_byte_i == 4'd0) tx_byte = core_pkg::REPLY_FIRST;
    else if (op == core_pkg::OP_REC_A && tx_byte_i >= 4'd3) tx_byte = rd_q[7:0];
    else if (op == core_pkg::OP_REC_B && tx_byte_i >= 4'd3) tx_byte = rd_q[15:8];
    else if (is_rec && tx_byte_i == 4'd1) tx_byte = seam_v ? {5'b0, 1'b1, snap_wp[9:8]} : 8'h00;
    else if (is_rec && tx_byte_i == 4'd2) tx_byte = seam_v ? snap_wp[7:0] : 8'h00;
    else if (op == core_pkg::OP_LIVE) begin
      if (tx_byte_i == 4'd2) tx_byte = snap_a; else if (tx_byte_i == 4'd4) tx_byte = snap_b;
    end else if (op == core_pkg::OP_DBG_STAT) case (tx_byte_i)
        4'd3:  tx_byte = core_pkg::DBG_HEAD;
        4'd4:  tx_byte = TAG;
        4'd5:  tx_byte = state;
        4'd6:  tx_byte = regs.tb;
        4'd7:  tx_byte = regs.src;
        4'd8:  tx_byte = regs.lvl;
        4'd9:  tx_byte = {6'b0, snap_wp[9:8]};
        4'd10: tx_byte = snap_wp[7:0];
        4'd11: tx_byte = nheld;
        4'd12: tx_byte = core_pkg::DBG_TAIL;
        4'd13: tx_byte = {1'b0, regs.t50b, 1'b0, regs.t50a};
        4'd14: tx_byte = {regs.hys, 2'b00, regs.edge_mode};
        4'd15: tx_byte = nlost;
        default: tx_byte = 8'h00;
      endcase
    else if (op == core_pkg::OP_IDENT) case (tx_byte_i)
        4'd2:  tx_byte = core_pkg::ID_MAGIC0;
        4'd3:  tx_byte = core_pkg::ID_MAGIC1;
        4'd4:  tx_byte = core_pkg::PROTO_VER;
        4'd5:  tx_byte = TAG;
        4'd6:  tx_byte = core_pkg::CAPS;
        4'd7:  tx_byte = ID_DEPTH;
        4'd8:  tx_byte = ID_CHECK;
        default: tx_byte = 8'h00;
      endcase
  end
endmodule
`default_nettype wire
