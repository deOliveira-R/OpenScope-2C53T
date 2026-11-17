`timescale 1ns/1ps
// Our simulation models (sim/cells_sim.sv, compiled with the prefix our_) against the vendor's library, cell by
// cell, under random stimulus. IDDRC, ODDRC and SDPB must be equal at every instant; for the PLL the output
// frequencies and the lock are compared, not the phase. CLEAR is not exercised (the core ties it low).
module cells_tb;
  reg clk = 0; always #4 clk = ~clk;
  reg clkb = 0; always #7.3 clkb = ~clkb;
  GSR GSR(.GSRI(1'b1));
  integer seed = 7, errs = 0, n = 0;
  // ---- IDDRC / ODDRC
  reg d = 0, d0 = 0, d1 = 0, clr = 0;
  always #1.7 d = $urandom(seed);                      // changes at any phase, both halves of the clock
  always @(posedge clk) begin #1 d0 = $urandom(seed); d1 = $urandom(seed); end
  wire vq0, vq1, oq0, oq1, vo, oo;
  IDDRC     vi (.Q0(vq0), .Q1(vq1), .D(d), .CLK(clk), .CLEAR(clr));
  our_IDDRC oi (.Q0(oq0), .Q1(oq1), .D(d), .CLK(clk), .CLEAR(clr));
  ODDRC     vd (.Q0(vo), .Q1(), .D0(d0), .D1(d1), .TX(1'b0), .CLK(clk), .CLEAR(clr));
  our_ODDRC od (.Q0(oo), .Q1(), .D0(d0), .D1(d1), .TX(1'b0), .CLK(clk), .CLEAR(clr));
  // ---- SDPB
  reg [15:0] di = 0; reg [9:0] wa = 0, ra = 0; reg cea = 0, ceb = 1; reg [1:0] be = 2'b11;
  always @(negedge clk) begin di = $urandom(seed); wa = $urandom(seed); cea = (($urandom(seed) % 4) != 0); be = (($urandom(seed) % 8) == 0) ? $urandom(seed) : 2'b11; end
  always @(negedge clkb) begin ra = $urandom(seed); ceb = (($urandom(seed) % 5) != 0); end
  wire [31:0] vdo, odo;
  SDPB #(.READ_MODE(1'b0), .BIT_WIDTH_0(16), .BIT_WIDTH_1(16), .BLK_SEL_0(3'b000), .BLK_SEL_1(3'b000), .RESET_MODE("SYNC"))
    vm (.DO(vdo), .DI({16'd0, di}), .BLKSELA(3'b000), .BLKSELB(3'b000), .ADA({wa, 2'b00, be}), .ADB({ra, 4'b0000}),
        .CLKA(clk), .CLKB(clkb), .CEA(cea), .CEB(ceb), .OCE(1'b1), .RESETA(1'b0), .RESETB(1'b0));
  our_SDPB #(.READ_MODE(1'b0), .BIT_WIDTH_0(16), .BIT_WIDTH_1(16), .BLK_SEL_0(3'b000), .BLK_SEL_1(3'b000), .RESET_MODE("SYNC"))
    om (.DO(odo), .DI({16'd0, di}), .BLKSELA(3'b000), .BLKSELB(3'b000), .ADA({wa, 2'b00, be}), .ADB({ra, 4'b0000}),
        .CLKA(clk), .CLKB(clkb), .CEA(cea), .CEB(ceb), .OCE(1'b1), .RESETA(1'b0), .RESETB(1'b0));
  // every edge counts too: a model must not make pulses the vendor's does not (a zero-width one would clock a converter)
  integer ev [0:3], eo [0:3];
  initial begin ev[0] = 0; ev[1] = 0; ev[2] = 0; ev[3] = 0; eo[0] = 0; eo[1] = 0; eo[2] = 0; eo[3] = 0; end
  always @(vq0) ev[0] = ev[0] + 1;  always @(vq1) ev[1] = ev[1] + 1;  always @(vo) ev[2] = ev[2] + 1;  always @(vdo) ev[3] = ev[3] + 1;
  always @(oq0) eo[0] = eo[0] + 1;  always @(oq1) eo[1] = eo[1] + 1;  always @(oo) eo[2] = eo[2] + 1;  always @(odo) eo[3] = eo[3] + 1;
  reg started = 0;
  integer e_i = 0, e_o = 0, e_m = 0;
  always @(vq0 or vq1 or oq0 or oq1 or vo or oo or vdo or odo) begin
    #0.01; n = n + 1;
    if (started) begin
      if ({vq0, vq1} !== {oq0, oq1}) begin e_i = e_i + 1; if (e_i < 4) $display("IDDRC differs at %0t: vendor %b%b ours %b%b", $time, vq0, vq1, oq0, oq1); end
      if (vo !== oo) begin e_o = e_o + 1; if (e_o < 4) $display("ODDRC differs at %0t: vendor %b ours %b", $time, vo, oo); end
      if (vdo[15:0] !== odo[15:0]) begin e_m = e_m + 1; if (e_m < 4) $display("SDPB differs at %0t: vendor %h ours %h", $time, vdo[15:0], odo[15:0]); end
    end
  end
  // ---- PLL
  reg clk25 = 0; always #20 clk25 = ~clk25;
  wire vl, ol, va, vb, vc, vdd, oa, ob, oc, odd;
  `define PLLCFG #(.FCLKIN("25"), .IDIV_SEL(0), .FBDIV_SEL(1), .ODIVA_SEL(10), .ODIVB_SEL(8), .ODIVC_SEL(4), .ODIVD_SEL(4), .PSB_COARSE(1), .PSC_COARSE(1), .PSD_COARSE(3))
  `define PLLPINS .CLKIN(clk25), .CLKFB(1'b0), .RESET(1'b0), .RESET_P(1'b0), .RESET_I(1'b0), .RESET_S(1'b0), .FBDSEL(6'b0), .IDSEL(6'b0), .ODSELA(7'b0), .ODSELB(7'b0), .ODSELC(7'b0), .ODSELD(7'b0), .DTA(4'b0), .DTB(4'b0), .ICPSEL(5'b0), .LPFRES(3'b0), .PSSEL(2'b0), .PSDIR(1'b0), .PSPULSE(1'b0), .ENCLKA(1'b1), .ENCLKB(1'b1), .ENCLKC(1'b1), .ENCLKD(1'b1)
  PLLO     `PLLCFG vp (`PLLPINS, .LOCK(vl), .CLKOUTA(va), .CLKOUTB(vb), .CLKOUTC(vc), .CLKOUTD(vdd));
  our_PLLO `PLLCFG op (`PLLPINS, .LOCK(ol), .CLKOUTA(oa), .CLKOUTB(ob), .CLKOUTC(oc), .CLKOUTD(odd));
  integer cv [0:3], co [0:3]; integer i;
  always @(posedge va) cv[0] = cv[0] + 1;  always @(posedge vb) cv[1] = cv[1] + 1;
  always @(posedge vc) cv[2] = cv[2] + 1;  always @(posedge vdd) cv[3] = cv[3] + 1;
  always @(posedge oa) co[0] = co[0] + 1;  always @(posedge ob) co[1] = co[1] + 1;
  always @(posedge oc) co[2] = co[2] + 1;  always @(posedge odd) co[3] = co[3] + 1;
  initial begin
    #100; started = 1;
    #200000;
    if (vl !== 1'b1 || ol !== 1'b1) begin $display("FAIL PLL lock: vendor %b ours %b", vl, ol); errs = errs + 1; end
    for (i = 0; i < 4; i = i + 1) begin cv[i] = 0; co[i] = 0; end
    #100000;                                                 // 100 us: 5000 / 6250 / 12500 / 12500 periods
    $display("PLL periods in 100 us  vendor: %0d %0d %0d %0d   ours: %0d %0d %0d %0d", cv[0], cv[1], cv[2], cv[3], co[0], co[1], co[2], co[3]);
    for (i = 0; i < 4; i = i + 1) if (cv[i] - co[i] > 1 || co[i] - cv[i] > 1) begin $display("FAIL PLL output %0d frequency", i); errs = errs + 1; end
    #400000;
    $display("cells: %0d comparisons; differences: IDDRC %0d, ODDRC %0d, SDPB %0d", n, e_i, e_o, e_m);
    $display("output changes  vendor: IDDRC %0d/%0d ODDRC %0d SDPB %0d   ours: %0d/%0d %0d %0d", ev[0], ev[1], ev[2], ev[3], eo[0], eo[1], eo[2], eo[3]);
    for (i = 0; i < 4; i = i + 1) if (ev[i] != eo[i]) begin $display("FAIL output %0d: different number of changes (glitches)", i); errs = errs + 1; end
    if (errs == 0 && e_i == 0 && e_o == 0 && e_m == 0) $display("CELLS PASS"); else $display("CELLS FAIL");
    $finish;
  end
endmodule
