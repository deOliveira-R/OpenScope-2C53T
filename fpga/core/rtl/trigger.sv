// Trigger: the source converter crossing the level.
//   either direction: the comparison "sample >= level" changed between two stored samples;
//   rising only:  the first stored sample >= level after one < level - H  (H = hysteresis);
//   falling only: the first stored sample <  level after one >= level + H.
// Without the hysteresis, noise on a slow edge fires the trigger on the wrong slope.
// The comparisons are registered in the cycle the pair moves from *_q to *_s, so "ge" belongs to the sample the
// memory is about to store.
`default_nettype none
module trigger (
  input  wire        clk,
  input  wire        we,           // a sample is being stored this cycle
  input  wire [7:0]  cha_q,
  input  wire [7:0]  chb_q,
  input  wire        src,          // 0 = converter A, 1 = B
  input  wire [7:0]  lvl,
  input  wire [3:0]  hys,
  input  wire [1:0]  edge_mode,
  output wire        fire          // the sample being stored is the trigger crossing
);
  wire  [7:0] ts = src ? chb_q : cha_q;
  logic [8:0] lvl_lo = 9'h080, lvl_hi = 9'h080;        // level -/+ hysteresis; bit 8 set = outside 0..255
  logic ge = 1'b0, gl = 1'b0, gh = 1'b0, prev_ge = 1'b0;
  logic arm_r = 1'b0, arm_f = 1'b0;
  always_ff @(posedge clk) begin
    ge <= (ts >= lvl);
    gl <= lvl_lo[8] || (ts >= lvl_lo[7:0]);            // "not below level - H"
    gh <= !lvl_hi[8] && (ts >= lvl_hi[7:0]);           // "at or above level + H"
    lvl_lo <= {1'b0, lvl} - {5'b0, hys};
    lvl_hi <= {1'b0, lvl} + {5'b0, hys};
    if (we) begin
      prev_ge <= ge;
      // armed for a rising trigger once the signal has been below level - H; the first sample at or above the
      // level then fires and disarms. Mirror image for falling.
      if (ge) arm_r <= 1'b0; else if (!gl) arm_r <= 1'b1;
      if (!ge) arm_f <= 1'b0; else if (gh) arm_f <= 1'b1;
    end
  end
  assign fire = edge_mode[1] ? (edge_mode[0] ? (arm_f && !ge) : (arm_r && ge)) : (prev_ge != ge);
endmodule
`default_nettype wire
