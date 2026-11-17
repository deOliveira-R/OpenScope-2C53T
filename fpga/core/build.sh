#!/bin/sh
# Capture core for the FNIRSI 2C53T: open flow (Yosys, nextpnr-himbaechel, Apicula) -> build/top.fs
#   sh build.sh            build
#   sh build.sh sim        run the testbench (needs Icarus Verilog and GOWIN_SIMLIB, see README.md)
# Tools are taken from PATH unless YOSYS, NEXTPNR, GOWIN_PACK or IVERILOG say otherwise. FREQ = the target in MHz.
cd "$(dirname "$0")" || exit 1
mkdir -p build
if [ "$1" = sim ]; then
  test -f "$GOWIN_SIMLIB" || { echo "set GOWIN_SIMLIB to the vendor's simlib/gw1n/prim_sim.v"; exit 1; }
  rm -f build/tb.vvp
  ${IVERILOG:-iverilog} -g2012 -s tb -o build/tb.vvp tb.v top.v "$GOWIN_SIMLIB" || exit 1
  exec vvp -n build/tb.vvp
fi
# nextpnr's constraint reader wants one statement per line and no comments
python3 -c "import re; t=re.sub(r'//[^\n]*','',open('pins.cst').read()); print('\n'.join(x.strip()+';' for x in t.split(';') if x.strip()))" > build/pins_open.cst
${YOSYS:-yosys} -p "read_verilog -lib pllo_bb.v; read_verilog top.v; synth_gowin -top top -json build/top.json" > build/yosys.log 2>&1 \
  || { echo "FAIL yosys"; tail -5 build/yosys.log; exit 1; }
${NEXTPNR:-nextpnr-himbaechel} --device GW1N-UV2QN48C7/I6 --vopt family=GW1N-2 --vopt cst=build/pins_open.cst \
  --freq ${FREQ:-125} --sdc clocks.sdc --json build/top.json --write build/pnr.json > build/pnr.log 2>&1 \
  || { echo "FAIL nextpnr"; grep -iE 'error' build/pnr.log | head -5; exit 1; }
${GOWIN_PACK:-gowin_pack} -d GW1N-2 --sspi_as_gpio --mspi_as_gpio --i2c_as_gpio -o build/top.fs build/pnr.json > build/pack.log 2>&1 \
  || { echo "FAIL pack"; tail -5 build/pack.log; exit 1; }
grep -E "Max frequency for clock" build/pnr.log | tail -2
echo "ok: build/top.fs"
