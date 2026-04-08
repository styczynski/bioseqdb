#!/bin/bash

TYPE=${1:-1}
LENGTH=${2:-1100}
N=${3:-10000}
G_SIZE=${4:-1}
CROSSTALK=${5:-88}
THRES=${6:-5}

t=1

if [[ "$1" == *"PBSIM"* ]]; then
	if [ "$TACC" -eq "1" ]; then
		cd $DATA/PBSIM/src
	else
		cd ../PBSIM/src
	fi
	./run.sh 2
	./prepare_reads.py
	t=0
	printf "\nDon't forget to convert the PBSIM reads\n\n"
fi

if [ $t == 0 ]; then
	exit 0
fi

if [ "$TACC" -eq "1" ]; then
	src="$DATA/DAZZ_DB-master/human"
	printf "Running on TACC\n"
	cd $src
	./generate $TYPE $LENGTH $N $G_SIZE $CROSSTALK $THRES
else
	src="../DAZZ_DB-master/human"
	printf "Running on ce-cuda\n"
	cd $src
	./generate $TYPE $LENGTH $N $G_SIZE $CROSSTALK $THRES
fi

