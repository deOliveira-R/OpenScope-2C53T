`timescale 1ns/1ps
// Simulation models of the four Gowin cells the capture core uses, written for OpenScope from the cells'
// documented behaviour and checked against the vendor's simulation library (tb/cells_tb.sv). With these the
// testbenches run in Icarus Verilog alone.
// Only what the core uses is modelled; anything else stops the simulation instead of guessing.
`ifdef CELL_PREFIX_OUR
  `define CELL(n) our_``n
`else
  `define CELL(n) n
`endif

// Input DDR register with asynchronous clear. Q0 = the sample taken on the rising edge, Q1 = the one taken on
// the falling edge; both appear together on the next rising edge.
module `CELL(IDDRC) (output reg Q0, output reg Q1, input wire D, input wire CLK, input wire CLEAR);
  parameter Q0_INIT = 1'b0;
  parameter Q1_INIT = 1'b0;
  reg d_rise, d_fall;
  initial begin Q0 = Q0_INIT; Q1 = Q1_INIT; d_rise = 1'b0; d_fall = 1'b0; end
  always @(posedge CLK or posedge CLEAR)
    if (CLEAR) begin d_rise <= 1'b0; Q0 <= 1'b0; Q1 <= 1'b0; end
    else begin d_rise <= D; Q0 <= d_rise; Q1 <= d_fall; end
  always @(negedge CLK or posedge CLEAR)
    if (CLEAR) d_fall <= 1'b0; else d_fall <= D;
endmodule

// Output DDR register with asynchronous clear. D0 and D1 are taken on a rising edge; two cycles later D0 goes
// out while the clock is high and D1 while it is low. Q1 is the tri-state control (TX), not modelled beyond a
// register.
module `CELL(ODDRC) (output wire Q0, output wire Q1, input wire D0, input wire D1, input wire TX, input wire CLK, input wire CLEAR);
  parameter TXCLK_POL = 1'b0;
  parameter INIT = 1'b0;
  reg d0_a, d0_b, d1_a, d1_b, d1_c, q, tx_a, tx_b;
  initial begin d0_a = INIT; d0_b = INIT; d1_a = INIT; d1_b = INIT; d1_c = INIT; q = INIT; tx_a = 1'b0; tx_b = 1'b0; end
  always @(posedge CLK or posedge CLEAR)
    if (CLEAR) begin d0_a <= 1'b0; d0_b <= 1'b0; d1_a <= 1'b0; d1_b <= 1'b0; d1_c <= 1'b0; tx_a <= 1'b0; tx_b <= 1'b0; end
    else begin d0_a <= D0; d0_b <= d0_a; d1_a <= D1; d1_b <= d1_a; d1_c <= d1_b; tx_a <= TX; tx_b <= tx_a; end
  // one output register written on both edges: no glitch when the two halves hold the same value
  always @(posedge CLK or negedge CLK or posedge CLEAR)
    if (CLEAR) q <= 1'b0; else if (CLK) q <= d0_b; else q <= d1_c;
  assign Q0 = q;
  assign Q1 = tx_b;
endmodule

// Semi-dual-port block RAM, as the core uses it: 16-bit words on both ports, output not registered
// (READ_MODE 0), synchronous reset. Port A writes (ADA[13:4] = word, ADA[1:0] = byte enables), port B reads
// (ADB[13:4] = word).
module `CELL(SDPB) (output reg [31:0] DO, input wire [31:0] DI, input wire [2:0] BLKSELA, input wire [2:0] BLKSELB,
             input wire [13:0] ADA, input wire [13:0] ADB, input wire CLKA, input wire CLKB,
             input wire CEA, input wire CEB, input wire OCE, input wire RESETA, input wire RESETB);
  parameter READ_MODE = 1'b0;
  parameter BIT_WIDTH_0 = 32;
  parameter BIT_WIDTH_1 = 32;
  parameter BLK_SEL_0 = 3'b000;
  parameter BLK_SEL_1 = 3'b000;
  parameter RESET_MODE = "SYNC";
  reg [15:0] mem [0:1023];
  integer k;
  initial begin
    if (READ_MODE !== 1'b0 || BIT_WIDTH_0 != 16 || BIT_WIDTH_1 != 16 || RESET_MODE != "SYNC") begin
      $display("SDPB model: only 16-bit, READ_MODE 0, SYNC reset is modelled"); $finish;
    end
    for (k = 0; k < 1024; k = k + 1) mem[k] = 16'd0;
    DO = 32'd0;
  end
  always @(posedge CLKA) if (CEA && BLKSELA == BLK_SEL_0) begin
    if (ADA[0]) mem[ADA[13:4]][7:0]  <= DI[7:0];
    if (ADA[1]) mem[ADA[13:4]][15:8] <= DI[15:8];
  end
  always @(posedge CLKB) if (CEB) begin
    if (RESETB) DO <= 32'd0;
    else if (BLKSELB == BLK_SEL_1) DO <= {16'd0, mem[ADB[13:4]]};
  end
endmodule

// PLL with four outputs, fixed dividers only. VCO = CLKIN x (FBDIV_SEL + 1) x ODIVA_SEL / (IDIV_SEL + 1);
// output X = VCO / ODIVX_SEL, starting PSX_COARSE VCO periods late (A has no shift). LOCK rises LOCK_NS after
// the input clock has been seen; the outputs run from then on.
module `CELL(PLLO) (
    input wire CLKIN, input wire CLKFB, input wire RESET, input wire RESET_P, input wire RESET_I, input wire RESET_S,
    input wire [5:0] FBDSEL, input wire [5:0] IDSEL,
    input wire [6:0] ODSELA, input wire [6:0] ODSELB, input wire [6:0] ODSELC, input wire [6:0] ODSELD,
    input wire [3:0] DTA, input wire [3:0] DTB, input wire [4:0] ICPSEL, input wire [2:0] LPFRES,
    input wire [1:0] PSSEL, input wire PSDIR, input wire PSPULSE,
    input wire ENCLKA, input wire ENCLKB, input wire ENCLKC, input wire ENCLKD,
    output reg LOCK, output reg CLKOUTA, output reg CLKOUTB, output reg CLKOUTC, output reg CLKOUTD);
  parameter FCLKIN = "100.0";
  parameter IDIV_SEL = 0;
  parameter FBDIV_SEL = 0;
  parameter ODIVA_SEL = 6;
  parameter ODIVB_SEL = 6;
  parameter ODIVC_SEL = 6;
  parameter ODIVD_SEL = 6;
  parameter PSB_COARSE = 1;
  parameter PSC_COARSE = 1;
  parameter PSD_COARSE = 1;
  parameter real LOCK_NS = 50000.0;
  realtime t_last = 0, t_in = 0, t_vco = 0;
  initial begin LOCK = 1'b0; CLKOUTA = 1'b0; CLKOUTB = 1'b0; CLKOUTC = 1'b0; CLKOUTD = 1'b0; end
  always @(posedge CLKIN) begin
    if (t_last != 0) begin t_in = $realtime - t_last; t_vco = t_in * (IDIV_SEL + 1) / ((FBDIV_SEL + 1) * ODIVA_SEL); end
    t_last = $realtime;
  end
  initial begin
    wait (t_vco != 0);
    #(LOCK_NS);
    @(posedge CLKIN);
    LOCK = 1'b1;
    fork
      forever begin CLKOUTA = 1'b1; #(t_vco * ODIVA_SEL / 2.0); CLKOUTA = 1'b0; #(t_vco * ODIVA_SEL / 2.0); end
      begin #(t_vco * PSB_COARSE); forever begin CLKOUTB = 1'b1; #(t_vco * ODIVB_SEL / 2.0); CLKOUTB = 1'b0; #(t_vco * ODIVB_SEL / 2.0); end end
      begin #(t_vco * PSC_COARSE); forever begin CLKOUTC = 1'b1; #(t_vco * ODIVC_SEL / 2.0); CLKOUTC = 1'b0; #(t_vco * ODIVC_SEL / 2.0); end end
      begin #(t_vco * PSD_COARSE); forever begin CLKOUTD = 1'b1; #(t_vco * ODIVD_SEL / 2.0); CLKOUTD = 1'b0; #(t_vco * ODIVD_SEL / 2.0); end end
    join
  end
endmodule

// The vendor library wants a global set/reset instance in the testbench; here it does nothing.
module `CELL(GSR) (input wire GSRI);
endmodule
