#!/bin/bash

# find PID of daligner/darwin
pid=$(ps -aux | grep "tqiu")
pid2=$(echo "$pid" | grep "reference_guided")
pid3=$(echo "$pid2" | awk '{print $2}')
echo $pid3

rm tmp_mem

# while process is active, find VmPeak in status file, store it in tmp
while [ -f /proc/$pid3/status ] ;
do
	grep VmRSS /proc/$pid3/status >> tmp_mem
	sleep 2
done

# remove duplicates in tmp and list
sort tmp_mem | uniq > memory_measurements
cat memory_measurements

