// Sample rate: one "tick" per stored sample. 125 MS/s divided by the timebase's divider, or, at 50 MS/s, the
// cycles in which converter A has a new value.
// The divider value is registered (it changes only with the timebase) and the tick is a flop, so the 125 MHz
// paths stay short.
`default_nettype none
module timebase (
  input  wire        clk,
  input  wire        cfg_ev,       // the settings changed this cycle
  input  wire [7:0]  tb,           // timebase code
  input  wire        new_a,        // 50 MS/s: converter A has a new value
  output logic       m50,          // converters at 50 MHz
  output logic       tick
);
  logic [17:0] div, div_r = 18'd25, reload = 18'd24, dcnt = 18'd0;
  logic        every = 1'b0;                           // divider 1: a tick in every cycle
  initial begin m50 = 1'b0; tick = 1'b1; end
  always_comb div = core_pkg::tb_divider(tb);
  always_ff @(posedge clk) begin
    div_r  <= div;
    m50    <= (tb == core_pkg::TB_50MS);
    reload <= div_r - 18'd1;
    every  <= (div_r == 18'd1);
    if (cfg_ev) begin dcnt <= 18'd7; tick <= 1'b0; end                  // a few cycles for the values above to settle
    else if (m50) tick <= new_a;
    else if (tick) begin dcnt <= reload; tick <= every; end
    else begin dcnt <= dcnt - 18'd1; tick <= (dcnt == 18'd1); end
  end
endmodule
`default_nettype wire
