// The record: a ring of 1024 sample pairs that keeps being written until a trigger stops it.
//   PRE    512 samples, so there is something before the trigger;
//   ARMED  writing, waiting for the trigger;
//   POST   511 more samples, then
//   HELD   nothing is written; the oldest sample is at the write pointer and the trigger crossing 512 after it.
// "restart" (a settings write, or the record has been read) starts over.
`default_nettype none
module capture (
  input  wire                           clk,
  input  wire                           tick,      // a sample is due
  input  wire                           fire,      // ... and it is the trigger crossing
  input  wire                           restart,
  output wire                           we,        // write the pair at wp
  output logic [core_pkg::ADDR_W-1:0]   wp,
  output core_pkg::cap_state_e          st,
  output wire                           held,
  output logic [7:0]                    nheld      // records held so far (debug)
);
  logic [core_pkg::ADDR_W-1:0] cnt = '0;
  initial begin wp = '0; st = core_pkg::ST_PRE; nheld = 8'd0; end
  assign held = (st == core_pkg::ST_HELD);
  assign we   = tick && !held;
  always_ff @(posedge clk) begin
    if (we) wp <= wp + 1'b1;
    if (restart) begin st <= core_pkg::ST_PRE; cnt <= '0; end
    else if (we) case (st)
        core_pkg::ST_PRE:   begin cnt <= cnt + 1'b1; if (cnt == core_pkg::PRE_LAST) st <= core_pkg::ST_ARMED; end
        core_pkg::ST_ARMED: if (fire) begin st <= core_pkg::ST_POST; cnt <= '0; end
        core_pkg::ST_POST:  begin cnt <= cnt + 1'b1; if (cnt == core_pkg::POST_LAST) begin st <= core_pkg::ST_HELD; nheld <= nheld + 1'b1; end end
        default: ;
      endcase
  end
endmodule
`default_nettype wire
