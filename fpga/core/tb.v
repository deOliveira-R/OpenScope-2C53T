`timescale 1ns/1ps
`ifndef TCO
`define TCO 3
`endif
`ifndef TA
`define TA 3'd0
`endif
`ifndef TB
`define TB 3'd0
`endif
// Testbench of the capture core: the firmware's frames at 60 MHz, a triangle on pins 18..27, the same +40 on
// pins 39..46 (the firmware's converter A). The converter model: its outputs change TCO ns after its clock rises.
// Run with -DTCO=n and -DTA=n -DTB=n (0..4) to sweep the output delay and the 50 MS/s sample cycles.
module tb;
  reg clk=0; always #20 clk=~clk;
  reg sclk=1, cs=1, mosi=1; wire miso, ca, cb, pc0;
  reg [7:0] a=8'h40, b=8'h68, ai=8'h40; reg up=1;
  always @(posedge ca) begin                    // triangle 0x30..0xD0, one code per sample
    if (up) begin if (ai == 8'hD0) up = 0; else ai = ai + 1; end
    else    begin if (ai == 8'h30) up = 1; else ai = ai - 1; end
    a <= #(`TCO) ai;
  end
  // Converter A (pins 39..46): samples as its clock rises, its outputs change after the clock FALLS (bench).
  reg [7:0] bi = 8'h68;
  always @(posedge cb) bi = ai + 8'd40;        // (the value before this edge's step when both clocks rise together)
  always @(negedge cb) b <= #(`TCO) bi;
  GSR GSR(.GSRI(1'b1));
  top #(.T50A(`TA), .T50B(`TB)) dut(clk, ca, cb, a, b, miso, sclk, cs, mosi, pc0, 1'b1);
  reg [7:0] rx [0:1100]; integer j, n, errs = 0;
  integer pc0_falls = 0; time t_fall = 0;
  always @(negedge pc0) begin pc0_falls = pc0_falls + 1; t_fall = $time; end
  task frame(input [7:0] op, input [7:0] val, input integer len);   // len = bytes after the opcode
    integer t; reg [7:0] o, r;
    begin
      cs=0; #20;
      for (t=0; t<len+1; t=t+1) begin
        o = (t==0) ? op : (t==1 ? val : 8'hFF);
        for (j=7; j>=0; j=j-1) begin sclk=0; mosi=o[j]; #8; sclk=1; r[j]=miso; #8; end
        rx[t]=r; #30;                             // the MCU polls byte by byte
      end
      #20; cs=1; #100;
    end
  endtask
  reg [7:0] A [0:1023]; reg [7:0] B [0:1023]; integer wp, seam, k, steps, f0; time t0;
  reg [7:0] ha1, ha2, hb1, hb2; integer hseam, v04 = 0, nv04 = 0, hdr_checked = 0;   // the seam bytes of the pair's two frames
  task pair(output integer handed);
    begin
      f0 = pc0_falls; t0 = $time;
      frame(8'h04, 8'hFF, 1026); for (k=0;k<1024;k=k+1) A[k]=rx[3+k]; ha1 = rx[1]; ha2 = rx[2];
      if (rx[0] !== 8'h80) begin $display("  FAIL s0=%h", rx[0]); errs=errs+1; end
      handed = (pc0_falls != f0);
      if (handed && !(t_fall > t0)) begin $display("  FAIL PC0 edge outside the op04 window"); errs=errs+1; end
      frame(8'h05, 8'hFF, 1026); for (k=0;k<1024;k=k+1) B[k]=rx[3+k]; hb1 = rx[1]; hb2 = rx[2];
      if (pc0_falls != f0 + (handed ? 1 : 0)) begin $display("  FAIL extra PC0 edge"); errs=errs+1; end
      hdr_rules(handed);
    end
  endtask
  // The seam bytes: never valid without a handed record; op 05 of a handed pair always valid; op 04's, when
  // valid, the same pointer; nothing but the valid bit and the pointer in them.
  task hdr_rules(input integer handed);
    begin
      if (ha1[7:3] !== 5'b0 || hb1[7:3] !== 5'b0) begin $display("  FAIL seam byte 1 has other bits: %h %h", ha1, hb1); errs=errs+1; end
      if (!handed && (ha1 !== 8'h00 || ha2 !== 8'h00 || hb1 !== 8'h00 || hb2 !== 8'h00)) begin $display("  FAIL seam bytes without a held record: %h %h / %h %h", ha1, ha2, hb1, hb2); errs=errs+1; end
      if (handed && !hb1[2]) begin $display("  FAIL op 05 of a held pair without a valid seam"); errs=errs+1; end
      if (handed && ha1[2] && {ha1[1:0], ha2} !== {hb1[1:0], hb2}) begin $display("  FAIL op 04 and op 05 give different seams: %h%h / %h%h", ha1, ha2, hb1, hb2); errs=errs+1; end
      hseam = {hb1[1:0], hb2};
      if (handed) begin if (ha1[2]) v04 = v04 + 1; else nv04 = nv04 + 1; end
    end
  endtask
  task check_record(input [7:0] lvl);
    integer bad, big, kk; reg [7:0] p, c;
    begin
      // seam = the one place where the ring is not continuous
      seam = -1; big = 0;
      for (k=0;k<1024;k=k+1) begin
        kk = (k+1) % 1024;
        steps = (A[kk] > A[k]) ? A[kk]-A[k] : A[k]-A[kk];
        if (steps > 1) begin big = big + 1; seam = kk; end
      end
      p = A[(seam+511)%1024]; c = A[(seam+512)%1024];
      // B[k] is the converter sample taken just after A[k]'s: the triangle is one step further, so A - B is 39
      // (rising) or 41 (falling), 40 at a turning point. One clock of misalignment gives 38 / 42.
      bad = 0; for (k=0;k<1024;k=k+1) begin kk = A[k] - B[k]; if (kk < 39 || kk > 41) bad = bad + 1; end
      $display("  record: seam at %0d (discontinuities %0d), samples at seam+511/+512: %h/%h, level %h, A - B outside 39..41 in %0d places",
               seam, big, p, c, lvl, bad);
      if (big != 1) begin $display("  FAIL not one seam"); errs=errs+1; end
      hdr_checked = hdr_checked + 1;
      if (hseam != seam) begin $display("  FAIL the seam bytes say %0d, the data %0d", hseam, seam); errs=errs+1; end
      if (!((p < lvl && c >= lvl) || (p >= lvl && c < lvl))) begin $display("  FAIL no crossing 512 after the seam"); errs=errs+1; end
      if (bad != 0) begin $display("  FAIL A/B not from the same addresses"); errs=errs+1; end
    end
  endtask
  task check_steps;                              // the triangle moves one code per converter sample
    integer zero, kk, big;
    begin
      zero = 0; for (k=0;k<1024;k=k+1) begin kk = (k+1) % 1024; if (A[kk] == A[k]) zero = zero + 1; end
      $display("  A: equal neighbours: %0d (the triangle's turning points only: at most 8)", zero);
      if (zero > 8) begin $display("  FAIL a converter A sample was stored twice"); errs=errs+1; end
      zero = 0; big = 0;
      for (k=0;k<1024;k=k+1) begin
        kk = (k+1) % 1024; if (B[kk] == B[k]) zero = zero + 1;
        steps = (B[kk] > B[k]) ? B[kk]-B[k] : B[k]-B[kk]; if (steps > 1) big = big + 1;
      end
      $display("  B: equal neighbours: %0d, steps of more than one code: %0d (the seam only)", zero, big);
      if (zero > 8) begin $display("  FAIL a converter B sample was stored twice"); errs=errs+1; end
      if (big != 1) begin $display("  FAIL a converter B sample was skipped"); errs=errs+1; end
    end
  endtask
  task check_dir(input [7:0] lvl, input integer rising);
    reg [7:0] p, c;
    begin
      p = A[(seam+511)%1024]; c = A[(seam+512)%1024];
      if (rising ? !(p < lvl && c >= lvl) : !(p >= lvl && c < lvl)) begin $display("  FAIL wrong direction at the trigger: %h -> %h", p, c); errs=errs+1; end
    end
  endtask
  integer h, m, racev, racen, racenh; reg [9:0] wpx;
  initial begin
    #300000;
    frame(8'hE0, 8'hFF, 13); $display("E0: %h %h state=%h tb=%h src=%h lvl=%h wp=%h%h held=%h end=%h", rx[3],rx[4],rx[5],rx[6],rx[7],rx[8],rx[9],rx[10],rx[11],rx[12]);
    if (rx[3] !== 8'hB5 || rx[12] !== 8'h5B || rx[5][7] !== 1'b1) begin $display("FAIL E0/lock"); errs=errs+1; end
    frame(8'h01, 8'h04, 1); frame(8'h02, 8'h03, 1); frame(8'h06, 8'h00, 1); frame(8'h07, 8'h00, 1); frame(8'h08, 8'h90, 1);
    frame(8'h41, 8'h00, 7);                       // a config-port read: must not be taken as "01 00"
    frame(8'hE0, 8'hFF, 13); $display("after the writes: tb=%h src=%h lvl=%h", rx[6],rx[7],rx[8]);
    if (rx[6] !== 8'h04 || rx[8] !== 8'h90) begin $display("FAIL registers"); errs=errs+1; end
    pair(h); $display("first pair right after the writes: handed over = %0d (expect 0, capture still filling)", h);
    #40000;
    for (n=0; n<3; n=n+1) begin
      pair(h); $display("pair %0d: handed over = %0d", n, h);
      if (h) check_record(8'h90); else begin $display("  FAIL expected a held record"); errs=errs+1; end
      #30000;
    end
    frame(8'h03, 8'hFF, 4); $display("op03: %h %h [A=%h] %h [B=%h]  (model now: A=%h B=%h)", rx[0],rx[1],rx[2],rx[3],rx[4], a, b);
    // slower rate, trigger on B, falling crossings count too
    frame(8'h01, 8'h06, 1); frame(8'h06, 8'h01, 1); frame(8'h08, 8'hA0, 1);
    #250000; pair(h); $display("25 MS/s, source B, level A0: handed over = %0d", h);
    if (h) begin
      seam=-1; for (k=0;k<1024;k=k+1) begin steps = (B[(k+1)%1024] > B[k]) ? B[(k+1)%1024]-B[k] : B[k]-B[(k+1)%1024]; if (steps > 6) seam=(k+1)%1024; end
      if (hseam != seam) begin $display("  FAIL the seam bytes say %0d, the data %0d", hseam, seam); errs=errs+1; end
      hdr_checked = hdr_checked + 1;
      $display("  seam %0d, B at seam+511/+512: %h/%h, step between samples about %0d codes", seam, B[(seam+511)%1024], B[(seam+512)%1024], B[(seam+2)%1024] > B[(seam+1)%1024] ? B[(seam+2)%1024]-B[(seam+1)%1024] : B[(seam+1)%1024]-B[(seam+2)%1024]);
    end else begin $display("  FAIL expected a held record"); errs=errs+1; end
    // a write drops a held record
    #250000; frame(8'h08, 8'hA1, 1); pair(h); $display("pair right after a level write: handed over = %0d (expect 0)", h);
    if (h) begin $display("  FAIL"); errs=errs+1; end
    // trigger edge (reg 07): rising only, then falling only
    frame(8'h01, 8'h04, 1); frame(8'h06, 8'h00, 1); frame(8'h08, 8'h90, 1); frame(8'h07, 8'h02, 1);
    #60000;
    for (m=0; m<2; m=m+1) begin
    if (m == 1) begin frame(8'h07, 8'h42, 1); #60000; $display("with hysteresis 4:"); end
    for (n=0; n<4; n=n+1) begin
      pair(h); $display("rising only, pair %0d: handed over = %0d", n, h);
      if (h) begin check_record(8'h90); check_dir(8'h90, 1); end else begin $display("  FAIL expected a held record"); errs=errs+1; end
      #(30000 + 700*n);
    end
    frame(8'h07, m ? 8'h43 : 8'h03, 1); #60000;
    for (n=0; n<4; n=n+1) begin
      pair(h); $display("falling only, pair %0d: handed over = %0d", n, h);
      if (h) begin check_record(8'h90); check_dir(8'h90, 0); end else begin $display("  FAIL expected a held record"); errs=errs+1; end
      #(30000 + 700*n);
    end
    end
    frame(8'hE0, 8'hFF, 13); $display("E0 state %h (edge bits = 11 expected)", rx[5]);
    if (rx[5][5:4] !== 2'b11) begin $display("FAIL edge bits"); errs=errs+1; end
    // 50 MS/s: the converters are clocked at 50 MHz, every converter sample is stored once
    frame(8'h07, 8'h00, 1); frame(8'h01, 8'h05, 1);
    #150000;
    for (n=0; n<3; n=n+1) begin
      pair(h); $display("50 MS/s, pair %0d: handed over = %0d", n, h);
      if (h) begin check_record(8'h90); check_steps; end else begin $display("  FAIL expected a held record"); errs=errs+1; end
      #80000;
    end
    // back to 125 MS/s: still one sample per clock
    frame(8'h01, 8'h04, 1); #60000; pair(h); $display("back at 125 MS/s: handed over = %0d", h);
    if (h) begin check_record(8'h90); check_steps; end else begin $display("  FAIL expected a held record"); errs=errs+1; end
    // The seam bytes against the core's own pointer, with the op 04 frame starting at every moment around the
    // instant the record becomes held (short frames: the pointer is compared, the samples are not read).
    frame(8'h07, 8'h02, 1); #60000; racev = 0; racen = 0; racenh = 0;
    for (n=0; n<700; n=n+1) begin
      f0 = pc0_falls;
      frame(8'h04, 8'hFF, 2); ha1 = rx[1]; ha2 = rx[2]; h = dut.pair_held; wpx = dut.wp;   // frozen while the pair lasts
      if (h !== (pc0_falls != f0)) begin $display("  FAIL race %0d: PC0 and the held pair disagree", n); errs=errs+1; end
      frame(8'h05, 8'hFF, 2); hb1 = rx[1]; hb2 = rx[2];
      if (h) begin
        if (!hb1[2] || {hb1[1:0], hb2} !== wpx) begin $display("  FAIL race %0d: op 05 seam %h%h, pointer %h", n, hb1, hb2, wpx); errs=errs+1; end
        if (ha1[2] && {ha1[1:0], ha2} !== wpx) begin $display("  FAIL race %0d: op 04 seam %h%h, pointer %h", n, ha1, ha2, wpx); errs=errs+1; end
        if (ha1[2]) racev = racev + 1; else racen = racen + 1;
      end else begin
        racenh = racenh + 1;
        if (ha1 !== 8'h00 || ha2 !== 8'h00 || hb1 !== 8'h00 || hb2 !== 8'h00) begin $display("  FAIL race %0d: seam bytes without a held pair", n); errs=errs+1; end
      end
      #(7000 + 5*n);
    end
    $display("race sweep, 700 pairs: op 04 seam valid in %0d, withheld in %0d held pairs (hold arrived as the frame began); %0d pairs not held", racev, racen, racenh);
    if (racev < 100) begin $display("FAIL the sweep did not exercise held pairs"); errs=errs+1; end
    $display("seam bytes checked against the data on %0d records; op 04 valid on %0d, withheld on %0d", hdr_checked, v04, nv04);
    if (errs == 0) $display("ALL CHECKS PASSED"); else $display("CHECKS FAILED: %0d", errs);
    $finish;
  end
endmodule
