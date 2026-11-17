// SPI slave, mode 3, one command per chip-select window. Knows nothing about the commands: it counts bits and
// bytes, hands each received byte over, and shifts out whatever byte it is given.
// MISO changes on the rising SCLK edge. Between frames it idles high: the first bit of every reply (80) waits
// on the line.
`default_nettype none
module spi_slave (
  input  wire        sclk,
  input  wire        cs_n,
  input  wire        mosi,
  output wire        miso,
  output logic [2:0] bit_i,        // bit within the byte, counted in rising edges
  output logic [3:0] byte_i,       // byte within the frame, stopping at 15 ("15 or later")
  output wire  [7:0] rx_byte,      // the byte received, valid on the edge where bit_i == 7
  output wire  [3:0] tx_byte_i,    // which byte the bit shifted out after this edge belongs to
  input  wire  [7:0] tx_byte       // ... and that byte
);
  logic [6:0] sh = 7'd0;
  logic       miso_r = 1'b1;
  initial begin bit_i = 3'd0; byte_i = 4'd0; end
  wire  [2:0] tx_bit_i = bit_i + 3'd1;
  assign rx_byte   = {sh, mosi};
  assign tx_byte_i = (bit_i == 3'd7 && byte_i != 4'd15) ? byte_i + 4'd1 : byte_i;
  always_ff @(posedge sclk or posedge cs_n)
    if (cs_n) begin bit_i <= 3'd0; byte_i <= 4'd0; sh <= 7'd0; end
    else begin
      sh <= {sh[5:0], mosi}; bit_i <= bit_i + 1'b1;
      if (bit_i == 3'd7 && byte_i != 4'd15) byte_i <= byte_i + 1'b1;
    end
  always_ff @(posedge sclk or posedge cs_n)
    if (cs_n) miso_r <= 1'b1;
    else miso_r <= tx_byte[3'd7 - tx_bit_i];
  assign miso = miso_r;
endmodule
`default_nettype wire
