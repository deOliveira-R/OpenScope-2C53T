// The two converters: their clocks out, their data in, and the pair of samples that belongs together.
//
// Names follow the firmware: converter A is the one op 04 reads (pins 39..46, clock on pin 47), converter B the
// one op 05 reads (pins 18..27, clock on pin 17).
//
// Clocks. A DDR output cell sends D0 while clk is high and D1 while it is low: (1,0) is the clock, (0,1) the
// inverted clock.
//   normal: 125 MHz, the two converters in antiphase (A on the clock, B on the inverted clock);
//   50 MS/s: a real 50 MHz on both, same edges. Five 8 ns cycles = ten 4 ns slots hold two periods, high in
//            slots 1,2 and 6,7.
//
// Data. Of the two samples one clock edge captures, converter B's was taken 0.454 of a period AFTER converter
// A's (bench, 10 MHz tone). That is the order the firmware's 250 MS/s merge wants (A[k], then B[k]), so the two
// go on as one pair.
//
// 50 MS/s. A converter's outputs change once per 20 ns and the capture register sees that as 2 or 3 equal
// values in a row. Each converter has its own two cycles of the five in which its value is taken (t50a, t50b
// choose them). The pair is complete when A's value has been taken (new_a); B's was taken one cycle before.
`default_nettype none
module adc_front (
  input  wire        clk,
  // pins
  output wire        cha_clk,
  output wire        chb_clk,
  input  wire [7:0]  cha_d,
  input  wire [7:0]  chb_d,
  // settings
  input  wire        m50,          // converters at 50 MHz
  input  wire [2:0]  t50a,         // 50 MS/s: first sample cycle of A (0..4); the second is 2 or 3 later
  input  wire [2:0]  t50b,
  // samples
  output logic [7:0] cha_q,        // the pair, as the trigger looks at it
  output logic [7:0] chb_q,
  output logic [7:0] cha_s,        // the same pair one cycle later, as the memory stores it
  output logic [7:0] chb_s,
  output logic       new_a         // 50 MS/s: this cycle holds a new sample of A
);
  // ---- data in
  wire [7:0] raw_a, raw_b;
  genvar i;
  generate for (i = 0; i < 8; i = i + 1) begin : g_in
    IDDRC ia (.Q0(raw_a[i]), .Q1(), .D(cha_d[i]), .CLK(clk), .CLEAR(1'b0));
    IDDRC ib (.Q0(raw_b[i]), .Q1(), .D(chb_d[i]), .CLK(clk), .CLEAR(1'b0));
  end endgenerate

  // ---- clocks out
  logic [2:0] ph = 3'd0;                               // which of the five cycles
  logic [1:0] pat;                                     // 50 MHz: {first half, second half} of this cycle
  logic a_d0 = 1'b1, a_d1 = 1'b0, b_d0 = 1'b0, b_d1 = 1'b1;
  always_comb case (ph)
      3'd0: pat = 2'b01;  3'd1: pat = 2'b10;  3'd3: pat = 2'b11;  default: pat = 2'b00;
    endcase
  always_ff @(posedge clk) begin
    ph <= (ph == 3'd4) ? 3'd0 : ph + 3'd1;
    {a_d0, a_d1} <= m50 ? pat : 2'b10;
    {b_d0, b_d1} <= m50 ? pat : 2'b01;
  end
  ODDRC oa (.Q0(cha_clk), .Q1(), .D0(a_d0), .D1(a_d1), .TX(1'b0), .CLK(clk), .CLEAR(1'b0));
  ODDRC ob (.Q0(chb_clk), .Q1(), .D0(b_d0), .D1(b_d1), .TX(1'b0), .CLK(clk), .CLEAR(1'b0));

  // ---- the sample pair
  logic [7:0] pa = 8'h80, pb = 8'h80;
  logic [2:0] t50a2 = 3'd0, t50b2 = 3'd0;              // each converter's second sample cycle
  logic       new_b = 1'b0;
  initial begin cha_q = 8'h80; chb_q = 8'h80; cha_s = 8'h80; chb_s = 8'h80; new_a = 1'b0; end
  always_ff @(posedge clk) begin
    pa <= raw_a; pb <= raw_b;
    cha_q <= pa; if (!m50 || new_b) chb_q <= pb;
    cha_s <= cha_q; chb_s <= chb_q;
    t50a2 <= (t50a >= 3'd3) ? t50a - 3'd3 : t50a + 3'd2;
    t50b2 <= (t50b >= 3'd3) ? t50b - 3'd3 : t50b + 3'd2;
    new_a <= (ph == t50a) || (ph == t50a2);
    new_b <= (ph == t50b) || (ph == t50b2);
  end
endmodule
`default_nettype wire
