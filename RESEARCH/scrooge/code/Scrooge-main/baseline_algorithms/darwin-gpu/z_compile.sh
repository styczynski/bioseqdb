#!/bin/bash

# compile with different flags for different optimizations

export nvccinstance=/usr/local/cuda-11.1/bin/nvcc

rm darwin

set -e

if [[ -n "$options" ]]; then
	printf "Error \$options was already set\n"
	exit 1
fi

options=""
maxregcount=128

for var in "$@"
do
	case "$var" in
	"GPU")
		options="$options -D GPU";;
	"TIME")
		options="$options -D TIME";;
	"NOSCORE")
		options="$options -D NOSCORE";;
	*)
		printf "Error unkown option '$var'\n"
		exit 1
	esac
done

if [ "$TACC" -eq "1" ]; then
	options="$options -D TACC=1"
	printf "Running on TACC\n"
else
	printf "Running on ce-cuda\n"
fi

old_options=$options
options="$options -D Z_COMPILE_USED"
gpu_options="$options --maxrregcount=$maxregcount"

# make 'options' visible to the makefile, note that 'echo $options' after the script is empty, because the script cannot set a variable in the parent environment
export options=$options
export gpu_options=$gpu_options

# compile
make clean
make

# let user know what options he used
printf "\nOptions used:\n$old_options\n"
head -n14 Makefile | grep "NCFLAGS" | grep -v "#"


