// 25 MHz reference -> the core's one clock, 125 MHz (PLL output C), and the reset that goes with it.
// rst is high from power-up, and again whenever the PLL loses lock; it goes low when the PLL has been
// locked for 255 clock cycles in a row (2 us). While it is high nothing is captured.
`default_nettype none
module clkgen (
  input  wire        clk25,
  output wire        clk,
  output wire        lock,
  output logic       rst,          // synchronous to clk
  output logic [7:0] nlost         // times the lock was lost since configuration (stops at 255)
);
  PLLO #(.FCLKIN("25"), .IDIV_SEL(0), .FBDIV_SEL(1), .ODIVA_SEL(10), .ODIVB_SEL(8), .ODIVC_SEL(4), .ODIVD_SEL(4),
         .PSB_COARSE(1), .PSC_COARSE(1), .PSD_COARSE(3)) pll (
    .CLKIN(clk25), .CLKFB(1'b0), .RESET(1'b0), .RESET_P(1'b0), .RESET_I(1'b0), .RESET_S(1'b0),
    .FBDSEL(6'b0), .IDSEL(6'b0), .ODSELA(7'b0), .ODSELB(7'b0), .ODSELC(7'b0), .ODSELD(7'b0),
    .DTA(4'b0), .DTB(4'b0), .ICPSEL(5'b0), .LPFRES(3'b0), .PSSEL(2'b0), .PSDIR(1'b0), .PSPULSE(1'b0),
    .ENCLKA(1'b1), .ENCLKB(1'b1), .ENCLKC(1'b1), .ENCLKD(1'b1),
    .LOCK(lock), .CLKOUTA(), .CLKOUTB(), .CLKOUTC(clk), .CLKOUTD());

  // rst_a is set by the loss of lock itself (no clock needed, so a short dip is caught) and released after 255
  // locked cycles. It has no fixed relation to clk when it is set, so it goes through two flops before anything
  // uses it: rst changes only on a clock edge, both ways.
  logic [7:0] ok_cnt = 8'd0;                           // cycles locked so far
  logic       rst_a  = 1'b1, rst_m = 1'b1;
  logic       rst_d  = 1'b1;                           // rst one cycle ago (1 at power-up: that reset is not a loss)
  initial begin rst = 1'b1; nlost = 8'd0; end
  always_ff @(posedge clk or negedge lock)
    if (!lock) begin ok_cnt <= 8'd0; rst_a <= 1'b1; end
    else if (ok_cnt != core_pkg::LOCK_OK_CYCLES) ok_cnt <= ok_cnt + 8'd1;
    else rst_a <= 1'b0;
  always_ff @(posedge clk) begin
    rst_m <= rst_a; rst <= rst_m;
    rst_d <= rst;
    if (rst && !rst_d && nlost != 8'hFF) nlost <= nlost + 8'd1;
  end
endmodule
`default_nettype wire
