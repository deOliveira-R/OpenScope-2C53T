// OpenScope capture core for the FNIRSI 2C53T (GW1N-UV2 QN48). See README.md for the picture; the SPI protocol
// is in cmd.sv and core_pkg.sv.
//
//   clkgen ── clk (125 MHz) ──┬─ adc_front ── sample pair ─┬─ trigger ── fire ─┐
//                             ├─ timebase ── tick ─────────┴───────────────────┴─ capture ── we, wp ── memory
//                             └─ cmd_sync ══ crossing ══ cmd ── spi_slave ── MCU          (read side: SCLK)
`default_nettype none
module top #(
  parameter logic [7:0] TAG  = 8'hD6,
  parameter logic [2:0] T50A = 3'd0,           // 50 MS/s: power-up sample cycles (bench-chosen)
  parameter logic [2:0] T50B = 3'd0
) (
  input  wire        clk25,        // pin 16
  output wire        cha_clk,      // pin 47: converter A (the one op 04 reads)
  input  wire [7:0]  cha_d,        // pins 39..46
  output wire        chb_clk,      // pin 17: converter B (op 05)
  input  wire [7:0]  chb_d,        // pins 27, 24..18
  output wire        spi_miso,     // pin 28 -> PB4
  input  wire        spi_sclk,     // pin 29 <- PB3
  input  wire        spi_cs_n,     // pin 34 <- PB6
  input  wire        spi_mosi,     // pin 35 <- PB5
  output wire        ready_n,      // pin 38 -> PC0
  input  wire        ctl30         // pin 30: not used
);
  wire clk, lock, rst; wire [7:0] nlost;
  clkgen u_clk (.clk25(clk25), .clk(clk), .lock(lock), .rst(rst), .nlost(nlost));

  // ---- 125 MHz side
  core_pkg::cfg_t cfg;
  wire cfg_ev, rearm_ev, force_ev, forced, m50, tick, new_a, fire, we, held;
  wire [7:0] cha_q, chb_q, cha_s, chb_s, nheld;
  wire [core_pkg::ADDR_W-1:0] wp;
  core_pkg::cap_state_e st;

  adc_front u_adc (.clk(clk), .cha_clk(cha_clk), .chb_clk(chb_clk), .cha_d(cha_d), .chb_d(chb_d),
                   .m50(m50), .t50a(cfg.t50a), .t50b(cfg.t50b),
                   .cha_q(cha_q), .chb_q(chb_q), .cha_s(cha_s), .chb_s(chb_s), .new_a(new_a));
  timebase  u_tb  (.clk(clk), .cfg_ev(cfg_ev), .tb(cfg.tb), .new_a(new_a), .m50(m50), .tick(tick));
  trigger   u_trg (.clk(clk), .we(we), .cha_q(cha_q), .chb_q(chb_q), .src(cfg.src[0]), .lvl(cfg.lvl),
                   .hys(cfg.hys), .edge_mode(cfg.edge_mode), .fire(fire));
  capture   u_cap (.clk(clk), .tick(tick), .fire(fire), .restart(cfg_ev || rearm_ev), .force_ev(force_ev),
                   .we(we), .wp(wp), .st(st), .held(held), .forced(forced), .nheld(nheld));

  // ---- SCLK side
  wire [2:0] bit_i; wire [3:0] byte_i, tx_byte_i; wire [7:0] rx_byte, tx_byte;
  wire [core_pkg::ADDR_W-1:0] rd_addr, snap_wp; wire [15:0] rd_data;
  core_pkg::cfg_t regs; core_pkg::cap_state_e snap_st;
  wire cfg_tog, rd04_tog, pair_op05, force_tog, snap_forced; wire [7:0] snap_a, snap_b;

  spi_slave u_spi (.sclk(spi_sclk), .cs_n(spi_cs_n), .mosi(spi_mosi), .miso(spi_miso),
                   .bit_i(bit_i), .byte_i(byte_i), .rx_byte(rx_byte), .tx_byte_i(tx_byte_i), .tx_byte(tx_byte));
  cmd #(.TAG(TAG), .T50A(T50A), .T50B(T50B)) u_cmd (
                   .sclk(spi_sclk), .cs_n(spi_cs_n), .bit_i(bit_i), .byte_i(byte_i), .rx_byte(rx_byte),
                   .tx_byte_i(tx_byte_i), .tx_byte(tx_byte), .rd_addr(rd_addr), .rd_data(rd_data),
                   .regs(regs), .cfg_tog(cfg_tog), .rd04_tog(rd04_tog), .pair_op05(pair_op05), .force_tog(force_tog),
                   .held(held), .lock(lock), .nheld(nheld), .nlost(nlost),
                   .snap_wp(snap_wp), .snap_st(snap_st), .snap_forced(snap_forced), .snap_a(snap_a), .snap_b(snap_b));
  cmd_sync #(.T50A(T50A), .T50B(T50B)) u_sync (
                   .clk(clk), .rst(rst), .cs_n(spi_cs_n), .regs(regs), .cfg_tog(cfg_tog), .rd04_tog(rd04_tog),
                   .pair_op05(pair_op05), .force_tog(force_tog), .cfg(cfg), .cfg_ev(cfg_ev), .rearm_ev(rearm_ev),
                   .force_ev(force_ev), .ready_n(ready_n), .wp(wp), .st(st), .forced(forced), .cha_s(cha_s), .chb_s(chb_s),
                   .snap_wp(snap_wp), .snap_st(snap_st), .snap_forced(snap_forced), .snap_a(snap_a), .snap_b(snap_b));

  // ---- record memory: 1024 x {B, A}, written at 125 MHz, read on SCLK
  SDPB mem (.DO(rd_data), .DI({chb_s, cha_s}), .BLKSELA(3'b000), .BLKSELB(3'b000),
            .ADA({wp, 4'b0011}), .ADB({rd_addr, 4'b0000}),
            .CLKA(clk), .CLKB(spi_sclk), .CEA(we), .CEB(1'b1), .OCE(1'b1), .RESETA(1'b0), .RESETB(1'b0));
  defparam mem.READ_MODE = 1'b0; defparam mem.BIT_WIDTH_0 = 16; defparam mem.BIT_WIDTH_1 = 16;
  defparam mem.BLK_SEL_0 = 3'b000; defparam mem.BLK_SEL_1 = 3'b000; defparam mem.RESET_MODE = "SYNC";
endmodule
`default_nettype wire
