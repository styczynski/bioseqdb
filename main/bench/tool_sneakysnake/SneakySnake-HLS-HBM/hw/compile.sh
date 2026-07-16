ulimit -s unlimited 
g++ -g -fopenmp -D_OPENMP -mcmodel=large -DHLS_NO_XIL_FPO_LIB -o action_HBM -DNO_SYNTH -I../../include -I../../../software/include  -I../include -I$XILINX_VIVADO/include  pipeline.cpp fetch_mem.cpp hw_action_hbm.cpp
