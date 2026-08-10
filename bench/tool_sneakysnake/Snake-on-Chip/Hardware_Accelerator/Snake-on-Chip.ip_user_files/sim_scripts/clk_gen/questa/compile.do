vlib work
vlib msim

vlib msim/xil_defaultlib

vmap xil_defaultlib msim/xil_defaultlib

vlog -work xil_defaultlib -64 \
"../../../../GateKeeper.srcs/sources_1/ip/clk_gen/clk_gen_clk_wiz.v" \
"../../../../GateKeeper.srcs/sources_1/ip/clk_gen/clk_gen.v" \


vlog -work xil_defaultlib "glbl.v"

