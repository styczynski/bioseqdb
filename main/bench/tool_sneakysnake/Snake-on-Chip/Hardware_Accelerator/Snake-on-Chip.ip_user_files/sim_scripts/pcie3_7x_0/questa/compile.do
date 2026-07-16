vlib work
vlib msim

vlib msim/xil_defaultlib

vmap xil_defaultlib msim/xil_defaultlib

vlog -work xil_defaultlib -64 \
"../../../../GateKeeper.srcs/sources_1/ip/pcie3_7x_0/source/pcie3_7x_0_pcie_7vx.v" \
"../../../../GateKeeper.srcs/sources_1/ip/pcie3_7x_0/source/pcie3_7x_0_pcie_bram_7vx.v" \
"../../../../GateKeeper.srcs/sources_1/ip/pcie3_7x_0/source/pcie3_7x_0_pcie_bram_7vx_8k.v" \
"../../../../GateKeeper.srcs/sources_1/ip/pcie3_7x_0/source/pcie3_7x_0_pcie_bram_7vx_16k.v" \
"../../../../GateKeeper.srcs/sources_1/ip/pcie3_7x_0/source/pcie3_7x_0_pcie_bram_7vx_cpl.v" \
"../../../../GateKeeper.srcs/sources_1/ip/pcie3_7x_0/source/pcie3_7x_0_pcie_bram_7vx_rep.v" \
"../../../../GateKeeper.srcs/sources_1/ip/pcie3_7x_0/source/pcie3_7x_0_pcie_bram_7vx_rep_8k.v" \
"../../../../GateKeeper.srcs/sources_1/ip/pcie3_7x_0/source/pcie3_7x_0_pcie_bram_7vx_req.v" \
"../../../../GateKeeper.srcs/sources_1/ip/pcie3_7x_0/source/pcie3_7x_0_pcie_init_ctrl_7vx.v" \
"../../../../GateKeeper.srcs/sources_1/ip/pcie3_7x_0/source/pcie3_7x_0_pcie_pipe_lane.v" \
"../../../../GateKeeper.srcs/sources_1/ip/pcie3_7x_0/source/pcie3_7x_0_pcie_pipe_misc.v" \
"../../../../GateKeeper.srcs/sources_1/ip/pcie3_7x_0/source/pcie3_7x_0_pcie_pipe_pipeline.v" \
"../../../../GateKeeper.srcs/sources_1/ip/pcie3_7x_0/source/pcie3_7x_0_pcie_top.v" \
"../../../../GateKeeper.srcs/sources_1/ip/pcie3_7x_0/source/pcie3_7x_0_pcie_force_adapt.v" \
"../../../../GateKeeper.srcs/sources_1/ip/pcie3_7x_0/source/pcie3_7x_0_pipe_clock.v" \
"../../../../GateKeeper.srcs/sources_1/ip/pcie3_7x_0/source/pcie3_7x_0_pipe_drp.v" \
"../../../../GateKeeper.srcs/sources_1/ip/pcie3_7x_0/source/pcie3_7x_0_pipe_eq.v" \
"../../../../GateKeeper.srcs/sources_1/ip/pcie3_7x_0/source/pcie3_7x_0_pipe_rate.v" \
"../../../../GateKeeper.srcs/sources_1/ip/pcie3_7x_0/source/pcie3_7x_0_pipe_reset.v" \
"../../../../GateKeeper.srcs/sources_1/ip/pcie3_7x_0/source/pcie3_7x_0_pipe_sync.v" \
"../../../../GateKeeper.srcs/sources_1/ip/pcie3_7x_0/source/pcie3_7x_0_pipe_user.v" \
"../../../../GateKeeper.srcs/sources_1/ip/pcie3_7x_0/source/pcie3_7x_0_pipe_wrapper.v" \
"../../../../GateKeeper.srcs/sources_1/ip/pcie3_7x_0/source/pcie3_7x_0_qpll_drp.v" \
"../../../../GateKeeper.srcs/sources_1/ip/pcie3_7x_0/source/pcie3_7x_0_qpll_reset.v" \
"../../../../GateKeeper.srcs/sources_1/ip/pcie3_7x_0/source/pcie3_7x_0_qpll_wrapper.v" \
"../../../../GateKeeper.srcs/sources_1/ip/pcie3_7x_0/source/pcie3_7x_0_rxeq_scan.v" \
"../../../../GateKeeper.srcs/sources_1/ip/pcie3_7x_0/source/pcie3_7x_0_gt_wrapper.v" \
"../../../../GateKeeper.srcs/sources_1/ip/pcie3_7x_0/source/pcie3_7x_0_gt_top.v" \
"../../../../GateKeeper.srcs/sources_1/ip/pcie3_7x_0/source/pcie3_7x_0_gt_common.v" \
"../../../../GateKeeper.srcs/sources_1/ip/pcie3_7x_0/source/pcie3_7x_0_gtx_cpllpd_ovrd.v" \
"../../../../GateKeeper.srcs/sources_1/ip/pcie3_7x_0/source/pcie3_7x_0_pcie_tlp_tph_tbl_7vx.v" \
"../../../../GateKeeper.srcs/sources_1/ip/pcie3_7x_0/source/pcie3_7x_0_pcie_3_0_7vx.v" \
"../../../../GateKeeper.srcs/sources_1/ip/pcie3_7x_0/sim/pcie3_7x_0.v" \


vlog -work xil_defaultlib "glbl.v"

