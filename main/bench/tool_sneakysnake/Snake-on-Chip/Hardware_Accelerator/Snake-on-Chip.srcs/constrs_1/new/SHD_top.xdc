set_property PACKAGE_PIN AV35 [get_ports PCIE_RESET_N]
set_property IOSTANDARD LVCMOS18 [get_ports PCIE_RESET_N]
set_property PULLUP true [get_ports PCIE_RESET_N]
# The following constraints are BOARD SPECIFIC. This is for the VC709
set_property LOC IBUFDS_GTE2_X1Y11 [get_cells refclk_ibuf]
create_clock -period 10.000 -name pcie_refclk_new [get_pins refclk_ibuf/O]
set_false_path -from [get_ports PCIE_RESET_N]

set_property IOSTANDARD LVCMOS18 [get_ports fpga_rst]
set_property PACKAGE_PIN AV40 [get_ports fpga_rst]

 set_false_path -from [get_cells genblk1[0].i_SHD_top/i_refread_manager/ref_reads_reg[*][*]]