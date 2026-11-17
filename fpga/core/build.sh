#!/bin/sh
# Capture core for the FNIRSI 2C53T: open flow (Yosys, nextpnr-himbaechel, Apicula) -> build/top.fs
#   sh build.sh            build
#   sh build.sh sim        run the testbench (Icarus Verilog)
#   sh build.sh cells      check our simulation models of the vendor cells against the vendor's library
#   sh build.sh lockstep   this core against core D4, pin for pin (a few minutes per seed)
# Tools are taken from PATH unless YOSYS, NEXTPNR, GOWIN_PACK or IVERILOG say otherwise. FREQ = the target in MHz.
# SEED = nextpnr's placement seed. The result varies a lot with it (clk 128..168 MHz over seeds 1..8, SCLK
# 66..79 MHz); the default is the best of those. A build that misses 125 / 62.5 MHz fails.
cd "$(dirname "$0")" || exit 1
mkdir -p build
if [ "$1" = sim ]; then
  # our own cell models by default; GOWIN_SIMLIB=<vendor>/simlib/gw1n/prim_sim.v uses the vendor's instead
  rm -f build/tb.vvp
  ${IVERILOG:-iverilog} -g2012 -s tb -o build/tb.vvp -c rtl/files.f tb/tb.sv "${GOWIN_SIMLIB:-sim/cells_sim.sv}" || exit 1
  exec vvp -n build/tb.vvp
fi
if [ "$1" = cells ]; then
  # our cell models against the vendor's library
  test -f "$GOWIN_SIMLIB" || { echo "set GOWIN_SIMLIB to the vendor's simlib/gw1n/prim_sim.v"; exit 1; }
  rm -f build/cells.vvp
  ${IVERILOG:-iverilog} -g2012 -DCELL_PREFIX_OUR -s cells_tb -o build/cells.vvp tb/cells_tb.sv sim/cells_sim.sv "$GOWIN_SIMLIB" || exit 1
  exec vvp -n build/cells.vvp
fi
if [ "$1" = lockstep ]; then
  # core D4 (ref/capcore_d4.v) and this core on the same pins under random traffic; SEEDS="1 2 3", FRAMES=400
  sed -e 's/^module top(/module top_d4(/' -e 's/always @\* case (tb)/always_comb case (tb)/' -e 's/always @\* case (ph)/always_comb case (ph)/' ref/capcore_d4.v > build/top_d4.v
  rm -f build/ls.vvp
  ${IVERILOG:-iverilog} -g2012 -s lockstep -o build/ls.vvp -c rtl/files.f tb/lockstep.sv build/top_d4.v "${GOWIN_SIMLIB:-sim/cells_sim.sv}" || exit 1
  rc=0
  for s in ${SEEDS:-1 2 3}; do vvp -n build/ls.vvp +seed=$s +frames=${FRAMES:-400} | grep -E "lockstep seed|LOCKSTEP|MISMATCH" | head -8; done | tee build/lockstep.log
  grep -q "LOCKSTEP FAIL" build/lockstep.log && rc=1
  grep -q "LOCKSTEP PASS" build/lockstep.log || rc=1
  exit $rc
fi
# nextpnr's constraint reader wants one statement per line and no comments
python3 -c "import re; t=re.sub(r'//[^\n]*','',open('pins.cst').read()); print('\n'.join(x.strip()+';' for x in t.split(';') if x.strip()))" > build/pins_open.cst
${YOSYS:-yosys} -p "read_verilog -lib rtl/pllo_bb.v; read_verilog -sv $(tr "\n" " " < rtl/files.f); synth_gowin -top top -json build/top.json" > build/yosys.log 2>&1 \
  || { echo "FAIL yosys"; tail -5 build/yosys.log; exit 1; }
${NEXTPNR:-nextpnr-himbaechel} --device GW1N-UV2QN48C7/I6 --vopt family=GW1N-2 --vopt cst=build/pins_open.cst \
  --freq ${FREQ:-125} --seed ${SEED:-7} --sdc clocks.sdc --json build/top.json --write build/pnr.json > build/pnr.log 2>&1 \
  || { echo "FAIL nextpnr"; grep -iE 'error' build/pnr.log | head -5; exit 1; }
# a constraint that names a net which no longer exists is silently dropped: refuse that
if grep -q "matched no objects" build/pnr.log; then echo "FAIL clocks.sdc names a net that does not exist:"; grep "matched no objects" build/pnr.log; exit 1; fi
${GOWIN_PACK:-gowin_pack} -d GW1N-2 --sspi_as_gpio --mspi_as_gpio --i2c_as_gpio -o build/top.fs build/pnr.json > build/pack.log 2>&1 \
  || { echo "FAIL pack"; tail -5 build/pack.log; exit 1; }
grep -E "Max frequency for clock" build/pnr.log | tail -2
echo "ok: build/top.fs"
