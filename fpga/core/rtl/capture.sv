// The record: a ring of 1024 sample pairs that keeps being written until a trigger stops it.
//   PRE    512 samples, so there is something before the trigger;
//   ARMED  writing, waiting for the trigger;
//   POST   511 more samples, then
//   HELD   nothing is written; the oldest sample is at the write pointer and the trigger crossing 512 after it.
// "restart" (a settings write, or the record has been read) starts over.
// "force" (op 09): the firmware has waited long enough for a trigger. The request is kept until the capture has
// been ARMED for 512 stored samples (with the 512 of PRE the whole ring is then newer than the restart); if no
// trigger has come by then the record is HELD as it is and marked forced: 1024 successive samples, both converters
// at the same moments, oldest at the write pointer, and no trigger crossing in it. A trigger always wins.
`default_nettype none
module capture (
  input  wire                           clk,
  input  wire                           tick,      // a sample is due
  input  wire                           fire,      // ... and it is the trigger crossing
  input  wire                           restart,
  input  wire                           force_ev,  // op 09 arrived (one cycle)
  output wire                           we,        // write the pair at wp
  output logic [core_pkg::ADDR_W-1:0]   wp,
  output core_pkg::cap_state_e          st,
  output wire                           held,
  output logic                          forced,    // the held record was forced, not triggered
  output logic [7:0]                    nheld      // records held so far (debug)
);
  logic [core_pkg::ADDR_W-1:0] cnt = '0;
  logic force_pend = 1'b0;
  initial begin wp = '0; st = core_pkg::ST_PRE; nheld = 8'd0; forced = 1'b0; end
  assign held = (st == core_pkg::ST_HELD);
  assign we   = tick && !held;
  always_ff @(posedge clk) begin
    if (we) wp <= wp + 1'b1;
    if (restart) begin st <= core_pkg::ST_PRE; cnt <= '0; force_pend <= 1'b0; forced <= 1'b0; end
    else begin
      if (force_ev) force_pend <= 1'b1;
      if (we) case (st)
        core_pkg::ST_PRE:   if (cnt == core_pkg::PRE_LAST) begin st <= core_pkg::ST_ARMED; cnt <= '0; end else cnt <= cnt + 1'b1;
        // cnt = samples stored since ARMED, stopping at PRE_LAST: from there on the ring is all new
        core_pkg::ST_ARMED: if (fire) begin st <= core_pkg::ST_POST; cnt <= '0; end
                            else if (cnt != core_pkg::PRE_LAST) cnt <= cnt + 1'b1;
                            else if (force_pend) begin st <= core_pkg::ST_HELD; forced <= 1'b1; nheld <= nheld + 1'b1; end
        core_pkg::ST_POST:  begin cnt <= cnt + 1'b1; if (cnt == core_pkg::POST_LAST) begin st <= core_pkg::ST_HELD; nheld <= nheld + 1'b1; end end
        default: ;
      endcase
    end
  end
endmodule
`default_nettype wire
