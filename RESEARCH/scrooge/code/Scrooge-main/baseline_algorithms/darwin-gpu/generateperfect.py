#!/usr/bin/python

# created 25-9-2018

import random

def generate(length):
	return ''.join(random.choice('CGTA') for _ in xrange(length))



# generate pairs of reads with an overlap
"""num_reads = 5000
length_read = 10000
min_overlap = 8000
max_overlap = 8000

f1 = open('reference.fasta', 'w')
f2 = open('reads.fasta', 'w')

for i in range(0, num_reads):
	overlap_length = random.randint(min_overlap, max_overlap)
	read1 = generate(length_read - overlap_length)
	overlap = generate(overlap_length)
	read1 += overlap
	read2 = overlap + generate(length_read - overlap_length)
	gen_pos = i * 2 * length_read
	header1 = ">G" + str(i) + "_" + str(gen_pos) + "_" + str(len(read1))
	gen_pos += length_read - overlap_length
	header2 = ">G" + str(i) + "_" + str(gen_pos) + "_" + str(len(read2))

	f1.write(header1 + "\n")
	l = list((read1[0+i:70+i] for i in range(0, len(read1), 70)))
	for chunk in l:
		f1.write(chunk)
		f1.write('\n')
	f2.write(header2 + "\n")
	l = list((read2[0+i:70+i] for i in range(0, len(read2), 70)))
	for chunk in l:
		f2.write(chunk)
		f2.write('\n')

f1.close()
f2.close()
#"""


# generate a genome and reads, to do ref-based alignment
"""num_reads = 4000
length_read = 1500
genome_length = 4000000

f1 = open('reference.fasta', 'w')
f2 = open('reads.fasta', 'w')

genome = generate(genome_length)
f1.write(">genome_0\n")
l = list((genome[0+i:70+i] for i in range(0, len(genome), 70)))
for chunk in l:
	f1.write(chunk)
	f1.write('\n')

for i in range(0, num_reads):
	start_pos = random.randint(0, genome_length - length_read)
	read = genome[start_pos: start_pos + length_read]
	header = ">G" + str(i) + "_" + str(start_pos) + "_" + str(length_read) + "\n"
	f2.write(header)
	l = list((read[0+i:70+i] for i in range(0, len(read), 70)))
	for chunk in l:
		f2.write(chunk)
		f2.write('\n')

f1.close()
f2.close()
#"""

# generate a genome, and two sets of reads, to do de-novo alignment
num_reads = 5000
length_read = 10000
genome_length = 4000000
genome = generate(genome_length)

f1 = open('reference.fasta', 'w')
f2 = open('reads.fasta', 'w')

for i in range(0, num_reads):
	start_pos1 = random.randint(0, genome_length - length_read)
	read1 = genome[start_pos1: start_pos1 + length_read]
	header1 = ">G" + str(i) + "_" + str(start_pos1) + "_" + str(length_read) + "\n"
	f1.write(header1)
	l = list((read1[0+i:70+i] for i in range(0, len(read1), 70)))
	for chunk in l:
		f1.write(chunk)
		f1.write('\n')
	start_pos2 = random.randint(0, genome_length - length_read)
	read2 = genome[start_pos2: start_pos2 + length_read]
	header2 = ">G" + str(i) + "_" + str(start_pos2) + "_" + str(length_read) + "\n"
	f2.write(header2)
	l = list((read2[0+i:70+i] for i in range(0, len(read2), 70)))
	for chunk in l:
		f2.write(chunk)
		f2.write('\n')

f1.close()
f2.close()
#"""



