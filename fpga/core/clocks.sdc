create_clock -period 8 -name clk [get_nets clk]
create_clock -period 16 -name sclk [get_nets u_cmd.sclk]
create_clock -period 40 -name clk25 [get_nets u_clk.clk25]
