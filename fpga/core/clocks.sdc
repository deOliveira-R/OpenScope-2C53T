create_clock -period 8 -name clk [get_nets clk]
create_clock -period 16 -name sclk [get_nets spi_sclk_IBUF_I_O]
create_clock -period 40 -name clk25 [get_nets clk25_IBUF_I_O]
