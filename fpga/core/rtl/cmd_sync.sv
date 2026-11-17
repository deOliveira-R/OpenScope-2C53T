// The 125 MHz end of the commands: everything that crosses between the SPI clock and the core clock goes
// through here.
//   SCLK -> clk:  the registers (still when their toggle arrives), "an op 04 started a pair", chip select.
//   clk -> SCLK:  a snapshot of the capture (write pointer, state, the latest samples), taken while chip select
//                 is high so that it is still for the whole frame.
// Also here: the PC0 pulse and the re-arm when a pair has been read.
// Reset (PLL not locked) acts as a settings change that lasts: the capture is held at its start and, on release,
// begins with whatever the registers hold then.
`default_nettype none
module cmd_sync #(
  parameter logic [2:0] T50A = 3'd0,
  parameter logic [2:0] T50B = 3'd0
) (
  input  wire                          clk,
  input  wire                          rst,         // PLL not locked: no capture; the registers are taken over on release
  // from the SCLK side
  input  wire                          cs_n,
  input  wire  core_pkg::cfg_t         regs,
  input  wire                          cfg_tog,
  input  wire                          rd04_tog,
  input  wire                          pair_op05,
  input  wire                          force_tog,
  // settings in the core clock
  output core_pkg::cfg_t               cfg,
  output wire                          cfg_ev,      // cfg changes at the end of this cycle
  output wire                          rearm_ev,    // the op 05 of a pair has ended
  output wire                          force_ev,    // an op 09 arrived
  // PC0
  output wire                          ready_n,     // idle high; 8 us low when a held record starts going out
  // snapshot for the SCLK side
  input  wire  [core_pkg::ADDR_W-1:0]  wp,
  input  wire  core_pkg::cap_state_e   st,
  input  wire                          forced,
  input  wire  [7:0]                   cha_s,
  input  wire  [7:0]                   chb_s,
  output logic [core_pkg::ADDR_W-1:0]  snap_wp,
  output core_pkg::cap_state_e         snap_st,
  output logic                         snap_forced,
  output logic [7:0]                   snap_a,
  output logic [7:0]                   snap_b
);
  logic [2:0] cfg_s = 3'b000, r04_s = 3'b000, cs_s = 3'b111, frc_s = 3'b000;
  logic [1:0] p05_s = 2'b00;
  logic [9:0] pc0   = 10'd0;                           // PC0 low while non-zero: 1023 cycles = 8.2 us
  initial begin
    cfg = {core_pkg::RST_TB, core_pkg::RST_SRC, core_pkg::RST_LVL, 2'b00, 4'd0, T50A, T50B};
    snap_wp = '0; snap_st = core_pkg::ST_PRE; snap_a = 8'h80; snap_b = 8'h80; snap_forced = 1'b0;
  end
  assign cfg_ev   = cfg_s[2] ^ cfg_s[1];
  assign rearm_ev = (cs_s[2:1] == 2'b01) && p05_s[1];
  assign force_ev = frc_s[2] ^ frc_s[1];
  assign ready_n  = (pc0 == 10'd0);
  always_ff @(posedge clk) begin
    // In reset the last stage takes the inverse: cfg_ev is then high in every cycle without another gate in its
    // path (it fans out to the whole capture), and the registers are taken over in the last cycle of the reset.
    cfg_s <= {rst ? ~cfg_s[1] : cfg_s[1], cfg_s[0], cfg_tog};
    r04_s <= {r04_s[1:0], rd04_tog}; cs_s <= {cs_s[1:0], cs_n}; frc_s <= {frc_s[1:0], force_tog};
    p05_s <= {p05_s[0], pair_op05};
    if (cfg_ev) cfg <= regs;
    if (rst) pc0 <= 10'd0; else if (r04_s[2] ^ r04_s[1]) pc0 <= 10'd1023; else if (pc0 != 10'd0) pc0 <= pc0 - 1'b1;
    if (cs_s[2]) begin snap_a <= cha_s; snap_b <= chb_s; snap_wp <= wp; snap_st <= st; snap_forced <= forced; end
  end
endmodule
`default_nettype wire
