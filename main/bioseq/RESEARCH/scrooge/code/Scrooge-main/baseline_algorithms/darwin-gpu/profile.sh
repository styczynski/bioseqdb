#!/bin/bash

mode=${1:-1}
T=${2:-1}
BLOCKS=${3:-32}
TPB=${4:-32}

OUT="-o 28.nvprof"
#OUT=

last="$(head -n20 Makefile | grep "NCFLAGS" | grep -v "#")"
if [[ $last == *"-lineinfo"* ]]; then
	printf "Compiled ok\n"
else
        printf "Error: not compiled with -lineinfo\n\n"
        exit 1
fi

if [[ "$mode" -ne "f" ]]; then
	if [[ "$mode" -ne "nf" ]]; then
		printf "Error: use [f|nf] for profiling mode as first argument\n\n"
		exit 1
	fi
fi

printf "Running with arguments $T $BLOCKS $TPB\n"
if [[ "$mode" == "nf" ]] ; then
	printf "Mode: non-file\n"
else
	printf "Mode: file\n"
fi

sleep 1

METRICS="--metrics "
M0="gld_transactions,gst_transactions,gld_efficiency,gst_efficiency,gst_throughput,gld_throughput"
#M1=",gst_requested_throughput,gld_requested_throughput"
#M2=",l2_read_transactions,l2_write_transactions"
#M3=""
M4=",sm_efficiency,achieved_occupancy"

#EVENTS="--events"
#E0=" inst_executed,gld_inst_8bit,gld_inst_32bit"
#E1=",__l1_global_load_transactions,__l1_global_store_transactions"
E2=""
#E3=",l2_subp0_read_sector_misses"
#E4=",l2_subp0_total_read_sector_queries"
E5=""

if [[ "$mode" == "nf" ]]; then
	nvprof $METRICS$M0$M1$M2$M3$M4 $EVENTS$E0$E1$E2$E3$E4$E5  ./reference_guided reference.fasta reads.fasta $T $BLOCKS $TPB 
else
	nvprof --analysis-metrics $OUT ./reference_guided reference.fasta reads.fasta $T $BLOCKS $TPB
fi


