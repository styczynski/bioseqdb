vlib work
vlib msim

vlib msim/fifo_generator_v13_0_1
vlib msim/xil_defaultlib

vmap fifo_generator_v13_0_1 msim/fifo_generator_v13_0_1
vmap xil_defaultlib msim/xil_defaultlib

vcom -work fifo_generator_v13_0_1 -64 \
"../../../ipstatic/fifo_generator_v13_0_1/simulation/fifo_generator_vhdl_beh.vhd" \
"../../../ipstatic/fifo_generator_v13_0_1/hdl/fifo_generator_v13_0_rfs.vhd" \

vcom -work xil_defaultlib -64 \
"../../../../GateKeeper.srcs/sources_1/ip/pcie_recv_fifo/sim/pcie_recv_fifo.vhd" \

