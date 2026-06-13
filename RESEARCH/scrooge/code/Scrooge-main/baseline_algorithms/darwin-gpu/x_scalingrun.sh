#!/bin/bash

set -x

function bench_and_compare {
	date
	./z_compile.sh
	./run.sh 8 1 1
	cat darwin.*.out | sort > tg
	rm darwin.*.out
	./z_compile.sh GPU CMAT 64 STREAM GASAL
	./benchmark.py 1 32 128
	./benchmark.py 1 32 64
	./benchmark.py 8 16 64
	./benchmark.py 8 32 64
	./benchmark.py 8 64 64
	cat darwin.*.out | sort > ts
	diff tg ts | head
	rm darwin.*.out
	rm tg ts
	date
}

function compare {
	./z_compile.sh GPU GASAL STREAM 64 CMAT
	./run.sh 8 32 64
	cat darwin.*.out | sort | uniq > tg
	rm darwin.*.out
	./z_compile.sh GPU GASAL STREAM 64 CMAT NOSCORE
	./run.sh 8 32 64
	cat darwin.*.out | sort | uniq > ts
	rm darwin.*.out
	head tg ts
}

function debug {
	./z_compile.sh
#	./z_compile.sh GPU CMAT 64 STREAM GASAL
	./run.sh 1 1 1 > tg
	./z_compile.sh GPU CMAT 64 STREAM GASAL
	./run.sh 1 1 1 > ts
}

function set_params {
	sed -i "s/^match =.*$/match = $1/" params.cfg
	sed -i "s/^mismatch =.*$/mismatch = $2/" params.cfg
	sed -i "s/^gap_open =.*$/gap_open = $3/" params.cfg
	sed -i "s/^gap_extend =.*$/gap_extend = $4/" params.cfg
}

function run {
	./z_compile.sh
	./run.sh 8 32 64
	cat darwin.*.out | sort | uniq > tg
	rm darwin.*.out
	./z_compile.sh BATCH GPU STREAM 64 CMAT CBASES
	./run.sh 8 32 64
	cat darwin.*.out | sort | uniq > ts
	rm darwin.*.out
	diff tg ts | head
	./z_compile.sh GPU GASAL STREAM 64
	./run.sh 8 32 64
	cat darwin.*.out | sort | uniq > ts
	rm darwin.*.out
	diff tg ts | head
	./z_compile.sh GPU GASAL STREAM 64 CMAT
	./run.sh 8 32 64
	cat darwin.*.out | sort | uniq > ts
	rm darwin.*.out
	diff tg ts | head
	./z_compile.sh GPU GASAL STREAM 64 CMAT CPBASES
	./run.sh 8 32 64
	cat darwin.*.out | sort | uniq > ts
	rm darwin.*.out
	diff tg ts | head
}

compare

