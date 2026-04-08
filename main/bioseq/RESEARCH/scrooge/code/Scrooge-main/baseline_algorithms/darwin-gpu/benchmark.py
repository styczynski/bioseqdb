#!/usr/bin/python

import re, subprocess
import os, sys, time

def parse(line):
	[a,b,c] = map(float, re.findall('\d+', line))
	return a*60 + b + c/1000;

def parse_tdiff(line):
	[a,b] = map(float, re.findall('\d+', line))
	return a + b/100;


if os.path.exists('time_benchmark'):
	os.remove('time_benchmark')
if os.path.exists('tmp_benchmark'):
	os.remove('tmp_benchmark')

with open('Makefile') as f:
	for line in f:
		if 'NCFLAGS' in line:
			if '#NCFLAGS' not in line:
				if '-O3' not in line:
					print 'Error probably not compiled with -O3'
				else:
					print 'Probably compiled with -O3'
				break

if len(sys.argv) < 4:
	print 'Error not enough arguments, usage: ./benchmark.py CPU_THREADS NUM_BLOCKS THREADS_PER_BLOCK'
	exit()

N = 5
vals = [0 for x in range(N)]
i = 0
o1 = open('time_benchmark', 'a')
exe = './run.sh'
cmd = exe + ' ' + str(sys.argv[1]) + ' ' + str(sys.argv[2]) + ' ' + str(sys.argv[3]) + ' >> tmp_benchmark'

print 'Using: ' + cmd

while i < N:
	subprocess.call(cmd, shell=True, stderr=o1)
	i += 1

i = 0
proc = subprocess.Popen('grep "real" time_benchmark', shell=True, stdout=subprocess.PIPE)

for line in iter(proc.stdout.readline, ''):
	vals[i] = parse(line)
	i += 1

print(vals)
total = 0.0
for i in range(0,N):
	total += vals[i]
print 'Average total runtime: {:.2f}'.format(total/N)
print 'Average total runtime: {0:d}m {1:.2f}s'.format(int(total/N/60), total/N%60)

#i = 0
#proc = subprocess.Popen('grep "tdiff" tmp_benchmark|grep -v "r"', shell=True, stdout=subprocess.PIPE)

#for line in iter(proc.stdout.readline, ''):
#	vals[i] = parse_tdiff(line)
#	i += 1

#print(vals)
#total = 0.0
#for i in range(0,N):
#	total += vals[i]
#print 'Average cuda tdiff: {:.2f}'.format(total/N)


print 'Notible lines in benchmark:'
proc = subprocess.Popen('grep -v "real\|user\|sys\|/tmp/new-h\|^$" time_benchmark', shell=True)
proc.wait()
proc = subprocess.Popen('grep -i "ERROR\|failed" tmp_benchmark', shell=True)
proc.wait()

"""if os.path.exists('time_benchmark'):
	os.remove('time_benchmark')
if os.path.exists('tmp_benchmark'):
	os.remove('tmp_benchmark')"""

print 'Used run command:'
print(cmd)
print("Ran {0} times".format(N))
print('\n\n')
time.sleep(1)
