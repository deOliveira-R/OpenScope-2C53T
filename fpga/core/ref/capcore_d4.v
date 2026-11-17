// Capture core v3 for the FNIRSI 2C53T (GW1N-UV2 QN48), speaking the SPI protocol the firmware already uses
// (summarised below and in README.md). Builds in the vendor tool and in the open flow.
//
// One clock: PLLO as in stock, output C = 125 MHz. The ADC clocks come from DDR output cells on that clock, so
// their edges can sit on a 4 ns grid:
//   every timebase but 0x05: 125 MHz, the two converters in antiphase (as stock);
//   0x05: a real 50 MHz (8 ns high, 12 ns low), both converters on the same edges. Stock keeps the converters
//         at 125 MS/s there and takes samples 16 and 24 ns apart in turn; here every sample is 20 ns apart.
// The firmware's converter A is the one on pins 39..46 and
// its B the one on pins 18..27; B[k] is sampled half a period after A[k] (see the pipeline below). One
// 1024 x 16 block RAM holds {B, A}, so the two records share every address.
//
// SPI mode 3, one command per chip-select window, opcode first, all 8 opcode bits decoded:
//   01 vv  timebase code     06 vv  trigger source (bit 0: 0 = A, 1 = B)     08 vv  trigger level (raw code)
//   07 vv  trigger edge: bits 1:0 = 00 either direction (what the firmware's boot write asks for), 10 rising
//          only, 11 falling only; bits 7:4 = hysteresis H in codes for the one-direction modes: a rising
//          trigger is the first sample >= level after a sample < level - H (falling: first < level after one
//          >= level + H). Without it noise on a slow edge fires the trigger on the wrong slope.
//   02 accepted, no effect.   A write to 01/06/07/08 drops the capture in flight.
//   04 / 05  record of converter A / B: reply byte 0 = 80, bytes 1-2 = the seam (below), then 1024 samples
//            from memory address 0. When the record is held the oldest sample is at the write pointer (the
//            seam) and the trigger crossing 512 samples after it.
//            Seam (new in v3; v2 sent 00 00): byte 1 = 00000 V s9 s8, byte 2 = s7..s0. V = 1: this frame reads
//            a held record whose oldest sample is at index s; the trigger crossing is at (s + 512) mod 1024.
//            V = 0: no pointer is given (no held record, or the record was held only after this frame had
//            begun: V and s come from one snapshot taken before the frame). The op 05 of a pair always has
//            V = 1 with the pair's pointer.
//   03       byte 2 = converter A now, byte 4 = converter B now.
//   E1 vv    debug: 50 MS/s sample cycles, bits 2:0 for converter A and 6:4 for B (0..4 each), to find the
//            converters' output delays on the bench.
//   E0       debug: B5, TAG, state (lock, pair held, edge[1:0], 00, capture state), timebase, source, level,
//            write pointer (2), records held so far, 5B.
// Pin 38 (PC0): idle high; one low pulse of 8 us starting inside an op 04 frame that found a held record.
// The record stays frozen until the op 05 frame that follows that op 04 has ended; then the core re-arms by
// itself (no hold time). Trigger: crossing of the level on the source converter, in the direction reg 07 asks.
// MISO changes on the rising SCLK edge (checked on the unit at 60 MHz).
module top(input clk25, output adc_clk_a, output adc_clk_b, input [7:0] adc_a, input [7:0] adc_b,
           output spi_miso, input spi_sclk, input spi_cs_n, input spi_mosi, output ready_n, input ctl30);
  parameter TAG = 8'hD4;
  parameter T50A = 3'd0, T50B = 3'd0;          // 50 MS/s: which two of the five clock cycles hold a new sample of each converter
  wire lock, clk;
  PLLO #(.FCLKIN("25"), .IDIV_SEL(0), .FBDIV_SEL(1), .ODIVA_SEL(10), .ODIVB_SEL(8), .ODIVC_SEL(4), .ODIVD_SEL(4),
         .PSB_COARSE(1), .PSC_COARSE(1), .PSD_COARSE(3)) pll (
    .CLKIN(clk25), .CLKFB(1'b0), .RESET(1'b0), .RESET_P(1'b0), .RESET_I(1'b0), .RESET_S(1'b0),
    .FBDSEL(6'b0), .IDSEL(6'b0), .ODSELA(7'b0), .ODSELB(7'b0), .ODSELC(7'b0), .ODSELD(7'b0),
    .DTA(4'b0), .DTB(4'b0), .ICPSEL(5'b0), .LPFRES(3'b0), .PSSEL(2'b0), .PSDIR(1'b0), .PSPULSE(1'b0),
    .ENCLKA(1'b1), .ENCLKB(1'b1), .ENCLKC(1'b1), .ENCLKD(1'b1),
    .LOCK(lock), .CLKOUTA(), .CLKOUTB(), .CLKOUTC(clk), .CLKOUTD());

  // ---------------------------------------------------------------- ADC
  wire [7:0] a0, b0;
  genvar i;
  generate for (i = 0; i < 8; i = i + 1) begin : g
    IDDRC ia(.Q0(a0[i]), .Q1(), .D(adc_a[i]), .CLK(clk), .CLEAR(1'b0));
    IDDRC ib(.Q0(b0[i]), .Q1(), .D(adc_b[i]), .CLK(clk), .CLEAR(1'b0));
  end endgenerate
  // ADC clocks. A DDR output cell sends D0 while clk is high and D1 while it is low. (0,1) is the inverted
  // clock, (1,0) the clock: the antiphase pair stock makes from its 180 degree PLL output.
  // 50 MHz: five 8 ns cycles = ten 4 ns slots hold two periods, high in slots 1,2 and 6,7.
  reg [2:0] ph = 0; reg m50 = 0; reg ca0 = 0, ca1 = 1, cb0 = 1, cb1 = 0; reg [1:0] pat;
  always @* case (ph)
      3'd0: pat = 2'b01;  3'd1: pat = 2'b10;  3'd3: pat = 2'b11;  default: pat = 2'b00;
    endcase
  always @(posedge clk) begin
    ph <= (ph == 3'd4) ? 3'd0 : ph + 3'd1;
    {ca0, ca1} <= m50 ? pat : 2'b01;
    {cb0, cb1} <= m50 ? pat : 2'b10;
  end
  ODDRC oa(.Q0(adc_clk_a), .Q1(), .D0(ca0), .D1(ca1), .TX(1'b0), .CLK(clk), .CLEAR(1'b0));
  ODDRC ob(.Q0(adc_clk_b), .Q1(), .D0(cb0), .D1(cb1), .TX(1'b0), .CLK(clk), .CLEAR(1'b0));
  // One pipeline stage: the samples and, beside them, "source sample >= level". A crossing is then a change of
  // that one bit between two stored samples.
  //
  // Which converter is which (bench): the firmware's converter A (op 04, CH1 in the two-channel setting,
  // trigger source 0) is the one on pins 39..46 (adc_b here); its converter B (op 05) is on pins 18..27.
  // Measured on a 10 MHz tone into both converters: of the two samples one clock edge captures, the pins-18..27
  // one was taken 0.454 of a period AFTER the pins-39..46 one. That is the order the firmware's 250 MS/s merge
  // wants (A[k], then B[k]), so the two go into the same memory word as they come. (v1 delayed A by one clock:
  // B then came 1.454 periods after A and the merged record had its image at -12 dBc; -37 dBc as paired here.)
  //
  // 50 MS/s: a converter's outputs change once per 20 ns and the capture register sees that as 2 or 3 equal
  // values in a row. Each converter has its own two cycles of the five in which its value is taken (ha, hb):
  // bench, 10 MHz tone: converter B's outputs move in the cycles of choice 0 and converter A's in those of
  // choice 1 (A's outputs come about one clock later than B's). Defaults = the choices furthest from those.
  // The pair is stored when A's value has been taken; B's was taken one cycle before.
  reg [7:0] pa = 8'h80, pb = 8'h80, qa = 8'h80, qb = 8'h80, sa = 8'h80, sb = 8'h80; reg ge = 0, gl = 0, gh = 0;
  reg [7:0] lvl = 8'h80; reg src = 0;
  reg [8:0] lvl_lo = 9'h080, lvl_hi = 9'h080;  // level -/+ hysteresis; bit 8 set = outside 0..255
  wire [7:0] ts = src ? qb : qa;
  reg ha = 0, hb = 0;                          // "this cycle holds a new 50 MS/s sample" of A / of B
  always @(posedge clk) begin
    pa <= b0; pb <= a0;
    qa <= pa; if (!m50 || hb) qb <= pb;
    sa <= qa; sb <= qb; ge <= (ts >= lvl);
    gl <= lvl_lo[8] || (ts >= lvl_lo[7:0]);    // "not below level - H"
    gh <= !lvl_hi[8] && (ts >= lvl_hi[7:0]);   // "at or above level + H"
  end

  // ---------------------------------------------------------------- SPI side (SCLK)
  // Position in the frame, counted in rising SCLK edges: bit3 = bit within the byte, byt = byte, stopping at 15.
  reg [2:0] bit3 = 0; reg [3:0] byt = 0; reg [6:0] sh = 0; reg [7:0] op = 0;
  reg [7:0] r_tb = 8'h08, r_src = 8'h00, r_lvl = 8'h80; reg [1:0] r_edge = 2'b00; reg [3:0] r_hys = 4'd0; reg [2:0] r_t50a = T50A, r_t50b = T50B; reg cfg_tog = 0;
  reg pair_held = 0, rd04_tog = 0;
  wire held;                                   // from the 125 MHz side
  reg h1 = 0, h2 = 0;
  wire [7:0] byte_in = {sh, spi_mosi};
  always @(posedge spi_sclk or posedge spi_cs_n)
    if (spi_cs_n) begin bit3 <= 3'd0; byt <= 4'd0; sh <= 7'd0; end
    else begin
      sh <= {sh[5:0], spi_mosi}; bit3 <= bit3 + 1'b1;
      if (bit3 == 3'd7 && byt != 4'd15) byt <= byt + 1'b1;
    end
  always @(posedge spi_sclk) begin
    h1 <= held; h2 <= h1;
    if (!spi_cs_n && byt == 4'd0 && bit3 == 3'd7) begin
      op <= byte_in;
      if (byte_in == 8'h04) begin pair_held <= h2; if (h2) rd04_tog <= ~rd04_tog; end
    end
    if (!spi_cs_n && byt == 4'd1 && bit3 == 3'd7) begin
      if (op == 8'h01) begin r_tb  <= byte_in; cfg_tog <= ~cfg_tog; end
      if (op == 8'h06) begin r_src <= byte_in; cfg_tog <= ~cfg_tog; end
      if (op == 8'h07) begin r_edge <= byte_in[1:0]; r_hys <= byte_in[7:4]; cfg_tog <= ~cfg_tog; end
      if (op == 8'hE1) begin r_t50a <= byte_in[2:0]; r_t50b <= byte_in[6:4]; cfg_tog <= ~cfg_tog; end
      if (op == 8'h08) begin r_lvl <= byte_in; cfg_tog <= ~cfg_tog; end
    end
  end

  // ---------------------------------------------------------------- capture (125 MHz)
  reg [2:0] cfg_s = 0, r04_s = 0, cs_s = 3'b111;
  reg [7:0] tb = 8'h08;
  wire cfg_ev = cfg_s[2] ^ cfg_s[1];
  reg [1:0] p05_s = 0;                         // "this frame is the op 05 of a held pair": still since byte 0
  wire rearm_ev = (cs_s[2:1] == 2'b01) && p05_s[1];
  // sample rate: divide 125 MS/s
  reg [17:0] div;
  always @* case (tb)
      8'h00, 8'h01, 8'h02, 8'h03, 8'h04: div = 18'd1;
      8'h05: div = 18'd2;                     // 50 MS/s: not used, the samples follow the ADC clock (ph)
      8'h06: div = 18'd5;      8'h07: div = 18'd10;     8'h08: div = 18'd25;     8'h09: div = 18'd50;
      8'h0A: div = 18'd100;    8'h0B: div = 18'd250;    8'h0C: div = 18'd500;    8'h0D: div = 18'd1000;
      8'h0E: div = 18'd2500;   8'h0F: div = 18'd5000;   8'h10: div = 18'd10000;  8'h11: div = 18'd25000;
      8'h12: div = 18'd50000;  8'h13: div = 18'd100000;
      default: div = 18'd250000;              // 0x14 and the roll codes: 500 S/s
    endcase
  // The divider value is registered (it changes only with the timebase) and the tick is a flop, so the
  // 125 MHz paths stay short.
  reg [17:0] dcnt = 0, div_r = 18'd25, rl0 = 18'd24; reg every = 0, tick = 1; reg [1:0] em = 0; reg [3:0] hys = 0; reg arm_r = 0, arm_f = 0;
  reg [2:0] ta = T50A, ta2 = 3'd0, tb_ = T50B, tb2 = 3'd0;   // each converter: two cycles of the five, 2 and 3 apart
  localparam PRE = 2'd0, ARMED = 2'd1, POST = 2'd2, HELD = 2'd3;
  reg [1:0] st = PRE; reg [9:0] wp = 0, cnt = 0; reg prev_ge = 0; reg [7:0] nheld = 0;
  wire xing = em[1] ? (em[0] ? (arm_f && !ge) : (arm_r && ge)) : (prev_ge != ge);
  assign held = (st == HELD);
  wire we = tick && !held;
  reg [9:0] pc0 = 0;                           // PC0 low while non-zero: 1023 cycles = 8.2 us
  reg [7:0] live_a = 8'h80, live_b = 8'h80; reg [9:0] wp_f = 0; reg [1:0] st_f = 0;
  always @(posedge clk) begin
    cfg_s <= {cfg_s[1:0], cfg_tog}; r04_s <= {r04_s[1:0], rd04_tog}; cs_s <= {cs_s[1:0], spi_cs_n};
    p05_s <= {p05_s[0], (op == 8'h05) && pair_held};
    if (cfg_ev) begin tb <= r_tb; src <= r_src[0]; lvl <= r_lvl; em <= r_edge; hys <= r_hys; ta <= r_t50a; tb_ <= r_t50b; end
    ta2 <= (ta >= 3'd3) ? ta - 3'd3 : ta + 3'd2; tb2 <= (tb_ >= 3'd3) ? tb_ - 3'd3 : tb_ + 3'd2;
    ha <= (ph == ta) || (ph == ta2); hb <= (ph == tb_) || (ph == tb2);
    // decimator
    div_r <= div; m50 <= (tb == 8'h05);
    rl0 <= div_r - 18'd1; every <= (div_r == 18'd1);
    if (cfg_ev) begin dcnt <= 18'd7; tick <= 1'b0; end                   // a few cycles for the values above to settle
    else if (m50) tick <= ha;
    else if (tick) begin dcnt <= rl0; tick <= every; end
    else begin dcnt <= dcnt - 18'd1; tick <= (dcnt == 18'd1); end
    // record
    lvl_lo <= {1'b0, lvl} - {5'b0, hys}; lvl_hi <= {1'b0, lvl} + {5'b0, hys};
    if (we) begin
      wp <= wp + 1'b1; prev_ge <= ge;
      // armed for a rising trigger once the signal has been below level - H; the first sample at or above the
      // level then fires (if the capture is waiting for it) and disarms. Mirror image for falling.
      if (ge) arm_r <= 1'b0; else if (!gl) arm_r <= 1'b1;
      if (!ge) arm_f <= 1'b0; else if (gh) arm_f <= 1'b1;
    end
    if (cfg_ev || rearm_ev) begin st <= PRE; cnt <= 10'd0; end
    else if (we) case (st)
        PRE:   begin cnt <= cnt + 1'b1; if (cnt == 10'd511) st <= ARMED; end
        ARMED: if (xing) begin st <= POST; cnt <= 10'd0; end
        POST:  begin cnt <= cnt + 1'b1; if (cnt == 10'd510) begin st <= HELD; nheld <= nheld + 1'b1; end end
        default: ;
      endcase
    // PC0 pulse
    if (r04_s[2] ^ r04_s[1]) pc0 <= 10'd1023; else if (pc0 != 10'd0) pc0 <= pc0 - 1'b1;
    // values for the SPI side, still while a frame runs
    if (cs_s[2]) begin live_a <= sa; live_b <= sb; wp_f <= wp; st_f <= st; end
  end
  assign ready_n = (pc0 == 10'd0);

  // ---------------------------------------------------------------- memory and read-out
  wire [2:0] nbit = bit3 + 3'd1;               // the bit to present after this edge ...
  wire [3:0] byi = (bit3 == 3'd7 && byt != 4'd15) ? byt + 4'd1 : byt;   // ... and its byte (15 = "15 or later")
  // radr = sample index of the NEXT byte: 0 while bytes 0..2 go out, then one more per byte.
  reg [9:0] radr = 0;
  always @(posedge spi_sclk or posedge spi_cs_n)
    if (spi_cs_n) radr <= 10'd0;
    else if (bit3 == 3'd7 && byt >= 4'd2) radr <= radr + 1'b1;
  wire [15:0] rq; reg [15:0] rw = 0;
  SDPB mem (.DO({rq[15:0]}), .DI({sb, sa}), .BLKSELA(3'b000), .BLKSELB(3'b000),
            .ADA({wp, 4'b0011}), .ADB({radr, 4'b0000}),
            .CLKA(clk), .CLKB(spi_sclk), .CEA(we), .CEB(1'b1), .OCE(1'b1), .RESETA(1'b0), .RESETB(1'b0));
  defparam mem.READ_MODE = 1'b0; defparam mem.BIT_WIDTH_0 = 16; defparam mem.BIT_WIDTH_1 = 16;
  defparam mem.BLK_SEL_0 = 3'b000; defparam mem.BLK_SEL_1 = 3'b000; defparam mem.RESET_MODE = "SYNC";
  always @(posedge spi_sclk) if (bit3 == 3'd6) rw <= rq;      // one edge before the next byte starts
  wire [7:0] state = {lock, pair_held, r_edge, 2'b00, st_f};
  wire seam_v = pair_held && (st_f == HELD);
  reg [7:0] cur_b;
  always @* begin
    cur_b = 8'h00;
    if (byi == 4'd0) cur_b = 8'h80;
    else if (op == 8'h04 && byi >= 4'd3) cur_b = rw[7:0];
    else if (op == 8'h05 && byi >= 4'd3) cur_b = rw[15:8];
    // the seam: valid only when the snapshot taken before this frame already showed the record held (wp_f and
    // st_f are one snapshot) and the frame is part of a held pair. Bit 7 of byte 1 goes out before pair_held
    // of an op 04 is known, so it is a constant 0.
    else if ((op == 8'h04 || op == 8'h05) && byi == 4'd1) cur_b = seam_v ? {5'b0, 1'b1, wp_f[9:8]} : 8'h00;
    else if ((op == 8'h04 || op == 8'h05) && byi == 4'd2) cur_b = seam_v ? wp_f[7:0] : 8'h00;
    else if (op == 8'h03) begin
      if (byi == 4'd2) cur_b = live_a; else if (byi == 4'd4) cur_b = live_b;
    end else if (op == 8'hE0) case (byi)
        4'd3: cur_b = 8'hB5;  4'd4: cur_b = TAG;  4'd5: cur_b = state;  4'd6: cur_b = r_tb;
        4'd7: cur_b = r_src;  4'd8: cur_b = r_lvl; 4'd9: cur_b = {6'b0, wp_f[9:8]}; 4'd10: cur_b = wp_f[7:0];
        4'd11: cur_b = nheld; 4'd12: cur_b = 8'h5B; 4'd13: cur_b = {1'b0, r_t50b, 1'b0, r_t50a};
        4'd14: cur_b = {r_hys, 2'b00, r_edge};
        default: cur_b = 8'h00;
      endcase
  end
  reg miso = 1;
  always @(posedge spi_sclk or posedge spi_cs_n)
    if (spi_cs_n) miso <= 1'b1;                // bit 0 of every reply (80) waits on the line
    else miso <= cur_b[3'd7 - nbit];
  assign spi_miso = miso;
endmodule
