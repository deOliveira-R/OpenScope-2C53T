`timescale 1ns/1ps
// Lockstep: core D4 (the one-file Verilog, module renamed top_d4) and the restructured core on the same pins,
// under random traffic. Every output must be equal at every instant.
//   +seed=N   random seed          +frames=N   number of frames
module lockstep;
  reg clk25 = 0; always #20 clk25 = ~clk25;
  reg sclk = 1, cs = 1, mosi = 1;
  wire miso_o, miso_n, cka_o, ckb_o, cka_n, ckb_n, rdy_o, rdy_n;
  reg [7:0] da = 8'h40, db = 8'h68;            // converter A (pins 39..46) and B (pins 18..27) outputs
  GSR GSR(.GSRI(1'b1));
  // old port names: adc_clk_a/adc_a = pins 17, 18..27 = the firmware's converter B
  top_d4 old_c (.clk25(clk25), .adc_clk_a(ckb_o), .adc_clk_b(cka_o), .adc_a(db), .adc_b(da),
                .spi_miso(miso_o), .spi_sclk(sclk), .spi_cs_n(cs), .spi_mosi(mosi), .ready_n(rdy_o), .ctl30(1'b1));
  top #(.TAG(8'hD4))
      new_c (.clk25(clk25), .cha_clk(cka_n), .cha_d(da), .chb_clk(ckb_n), .chb_d(db),
             .spi_miso(miso_n), .spi_sclk(sclk), .spi_cs_n(cs), .spi_mosi(mosi), .ready_n(rdy_n), .ctl30(1'b1));
  // -DNETLIST: a third copy, the Yosys netlist of the new core (module renamed top_syn, TAG D4). It must equal
  // the new core's source with no alignment at all.
  wire miso_s, cka_s, ckb_s, rdy_s;
`ifdef NETLIST
  top_syn syn_c (.clk25(clk25), .cha_clk(cka_s), .cha_d(da), .chb_clk(ckb_s), .chb_d(db),
                 .spi_miso(miso_s), .spi_sclk(sclk), .spi_cs_n(cs), .spi_mosi(mosi), .ready_n(rdy_s), .ctl30(1'b1));
  wire [3:0] vs = {miso_s, cka_s, ckb_s, rdy_s};
`else
  wire [3:0] vs = {miso_n, cka_n, ckb_n, rdy_n};
`endif

  // ---- comparison: on any change, let the instant settle, then compare
  integer errs = 0; longint unsigned checks = 0; reg aligned = 0;
  integer fmode = 0, nforce = 0, nforced = 0;
  initial if ($test$plusargs("force")) fmode = 1;
  wire [3:0] vo = {miso_o, cka_o, ckb_o, rdy_o}, vn = {miso_n, cka_n, ckb_n, rdy_n};
  integer errs_s = 0; reg lockd = 0;
  always @(vn or vs) begin
    #0.002;
    if (vn !== vs && new_c.lock === 1'b1) begin
      errs_s = errs_s + 1;
      if (errs_s <= 10) $display("NETLIST MISMATCH at %0t ps: source %b, netlist %b", $time, vn, vs);
    end
  end
  always @(vo or vn) begin
    #0.002;
    checks = checks + 1;
    if (aligned && !fmode && vo !== vn) begin
      errs = errs + 1;
      if (errs <= 10) $display("MISMATCH at %0t ps: old {miso,ckA,ckB,rdy} = %b, new = %b", $time, vo, vn);
    end
  end

  // ---- converters: outputs change a little after their clock; the waveform changes from time to time
  integer seed, seed0, mode = 0; reg [7:0] w = 8'h40; reg up = 1; integer slow = 0;
  function [7:0] next_sample(input integer dummy);
    begin
      case (mode)
        0: begin if (up) begin if (w >= 8'hD0) up = 0; else w = w + 1; end else begin if (w <= 8'h30) up = 1; else w = w - 1; end end
        1: w = $urandom(seed);                                              // noise, full range
        2: w = 8'h80 + ($urandom(seed) % 7) - 3;                            // small noise around mid-scale
        3: begin slow = slow + 1; if (slow % 97 == 0) w = w + 1; end        // slow ramp
        default: w = (($urandom(seed) % 40) == 0) ? ~w : w;                 // square with random period
      endcase
      next_sample = w;
    end
  endfunction
  always @(posedge cka_o) da <= #3 next_sample(0);
  always @(negedge ckb_o) db <= #3 w + 8'd40;

  // ---- SPI traffic
  integer nfr, f, t, j, len, per, kind, abort_at, nrec = 0, nheldpairs = 0, r;
  reg [7:0] op, val, o, rx, hb1; real jit;
  reg [7:0] rxn, hb1n;                         // the same bytes from the new core (hb1 is core D4's)
  task automatic frame(input [7:0] fop, input [7:0] fval, input integer flen, input integer half, input integer cut);
    // flen = bytes after the opcode; half = SCLK half period in 0.5 ns steps; cut >= 0: CS rises after that many bits
    integer nb; real h;
    begin
      h = half * 0.5; nb = 0;
      cs = 0; #(4 + ($urandom(seed) % 4000) * 0.01);                       // CS at any phase of the core clock
      begin : bits
        for (t = 0; t < flen + 1; t = t + 1) begin
          o = (t == 0) ? fop : (t == 1 ? fval : 8'hFF);
          for (j = 7; j >= 0; j = j - 1) begin
            if (nb == cut) disable bits;
            sclk = 0; mosi = o[j]; #(h); sclk = 1; rx[j] = miso_o; rxn[j] = miso_n; #(h); nb = nb + 1;
          end
          if (t == 1) begin hb1 = rx; hb1n = rxn; end
          if (($urandom(seed) % 3) == 0) #(($urandom(seed) % 60) * 1.0);    // the MCU's gaps between bytes
        end
      end
      sclk = 1; #(2 + ($urandom(seed) % 30)); cs = 1;
      #(20 + ($urandom(seed) % 400));
    end
  endtask
  initial begin
    if (!$value$plusargs("seed=%d", seed)) seed = 1;
    if (!$value$plusargs("frames=%d", nfr)) nfr = 400;
    seed0 = seed;
    #300000;
    // The new core holds its capture until the PLL has locked; D4 ran a few samples earlier. Bring D4's
    // history-dependent state to the new core's once (between two clock edges), restart both with a second write
    // and let the ring fill. From here on the two must be equal.
    frame(8'h01, 8'h04, 1, 16, -1);
    @(negedge old_c.clk);
    old_c.wp = new_c.u_cap.wp; old_c.prev_ge = new_c.u_trg.prev_ge; old_c.arm_r = new_c.u_trg.arm_r;
    old_c.arm_f = new_c.u_trg.arm_f; old_c.nheld = new_c.u_cap.nheld;
    frame(8'h08, 8'h80, 1, 16, -1);
    #20000; aligned = 1;
    for (f = 0; f < nfr; f = f + 1) begin
      kind = $urandom(seed) % 100;
      per  = (($urandom(seed) % 4) == 0) ? 17 + ($urandom(seed) % 40) : 16;  // half period 8 ns (60 MHz) or slower
      if (($urandom(seed) % 25) == 0) mode = $urandom(seed) % 5;
      // +force (netlist runs): op 09 in one frame out of eight. Core D4 does not know it, so the comparison with
      // D4 is off in such a run; the netlist against the source is what is checked.
      if (fmode && ($urandom(seed) % 8) == 0) begin frame(8'h09, $urandom(seed), $urandom(seed) % 3, per, -1); nforce = nforce + 1; end
      // ... and now and then the case op 09 is for: a level no sample reaches, fast timebase, time for the memory
      // to fill, the request (before or after that time), then the pair.
      if (fmode && ($urandom(seed) % 6) == 0) begin
        frame(8'h07, 8'h02 | ($urandom(seed) & 8'h01), 1, per, -1); frame(8'h01, $urandom(seed) % 5, 1, per, -1); frame(8'h08, 8'hFF, 1, per, -1);
        if (($urandom(seed) % 2) == 0) begin frame(8'h09, 8'hFF, 0, per, -1); nforce = nforce + 1; #(12000.0 + ($urandom(seed) % 4000)); end
        else begin #(($urandom(seed) % 14000) * 1.0); frame(8'h09, 8'hFF, 0, per, -1); nforce = nforce + 1; #(($urandom(seed) % 12000) * 1.0); end
        frame(8'h04, 8'hFF, (($urandom(seed) % 2) == 0) ? 1026 : 8, per, -1); nrec = nrec + 1;
        frame(8'h05, 8'hFF, (($urandom(seed) % 2) == 0) ? 1026 : 8, per, -1); if (hb1n[2]) nheldpairs = nheldpairs + 1; if (hb1n[2] && hb1n[3]) nforced = nforced + 1;
      end
      if (kind < 30) begin                                                  // a record pair
        len = (($urandom(seed) % 3) == 0) ? 1026 : 2 + ($urandom(seed) % 40);
        frame(8'h04, 8'hFF, len, per, -1); nrec = nrec + 1;
        if (($urandom(seed) % 10) != 0) begin frame(8'h05, 8'hFF, len, per, -1); if (hb1[2]) nheldpairs = nheldpairs + 1; if (hb1n[2] && hb1n[3]) nforced = nforced + 1; end
      end else if (kind < 45) begin                                         // timebase: fast ones mostly, sometimes any
        r = $urandom(seed) % 10;
        val = (r < 6) ? ($urandom(seed) % 8) : (r < 9) ? ($urandom(seed) % 21) : $urandom(seed);
        frame(8'h01, val, 1, per, -1);
      end else if (kind < 53) frame(8'h06, $urandom(seed), 1, per, -1);
      else if (kind < 63) begin val = $urandom(seed); if (($urandom(seed) % 2) == 0) val[3:2] = 0; frame(8'h07, val, 1, per, -1); end
      else if (kind < 73) frame(8'h08, (($urandom(seed) % 3) == 0) ? $urandom(seed) : 8'h60 + ($urandom(seed) % 64), 1, per, -1);
      else if (kind < 78) frame(8'h03, 8'hFF, 4, per, -1);
      else if (kind < 84) frame(8'hE0, 8'hFF, 15 + ($urandom(seed) % 4), per, -1);
      else if (kind < 88) begin val = $urandom(seed); val[2:0] = val[2:0] % 5; val[6:4] = val[6:4] % 5; frame(8'hE1, val, 1, per, -1); end
      else if (kind < 92) frame(8'h02, $urandom(seed), 1, per, -1);
      else if (kind < 96) begin op = $urandom(seed); if (op == 8'hE2 || op == 8'h09) op = 8'hE3; frame(op, $urandom(seed), $urandom(seed) % 8, per, -1); end   // any opcode but E2 and 09 (new since D4)
      else begin                                                           // a frame cut short at any bit
        r = $urandom(seed) % 6; op = (r == 0) ? 8'h04 : (r == 1) ? 8'h05 : (r == 2) ? 8'h01 : (r == 3) ? 8'h08 : (r == 4) ? 8'h07 : 8'hE0;
        frame(op, $urandom(seed), 6, per, $urandom(seed) % 40);
      end
      if (($urandom(seed) % 4) == 0) #(($urandom(seed) % 60000) * 1.0);     // idle, so captures complete
    end
    #20000;
    $display("lockstep seed %0d: %0d frames, %0d record reads, %0d held pairs, %0d comparisons, %0d mismatches",
             seed0, nfr, nrec, nheldpairs, checks, errs);
`ifdef NETLIST
    $display("netlist against source: %0d mismatches", errs_s);
`endif
    if (fmode) $display("(+force: %0d op 09 frames, %0d forced records read; core D4 not compared)", nforce, nforced);
    if (nheldpairs < 5) $display("(few held pairs in this seed: %0d)", nheldpairs);
    if (errs == 0 && errs_s == 0) $display("LOCKSTEP PASS"); else $display("LOCKSTEP FAIL");
    $finish;
  end
endmodule
