#!/usr/bin/python

# analyze all reads, and determine how many and which overlaps should be reported
# analyze reported overlaps
# compare ideal and reported overlaps

import re, subprocess
from operator import itemgetter

# parse and extract integers
def parse(line):
	return map(int, re.findall('\d+', line))


# analyze all reads
# data header: id, startpos, original length, erronous length
f1 = open('../NPBSS/ecoliGenome.fa.npbss_simulated_CLR_1.fa','r')
f2 = open('../NPBSS/ecoliGenome.fa.npbss_simulated_CLR_2.fa','r')
list1 = []		# contains info from f1
list2 = []		# contains info from f2

for line in f1:
	if ">" in line:
		#print(line)
		t = parse(line)
		list1.append(t)

for line in f2:
	if ">" in line:
		#print(line)
		t = parse(line)
		list2.append(t)

f1.close()
f2.close()

### cal max diff between perfect read length and erronous read length
"""list3 = []
max1 = 0
for t in list1:
	max1 = max(max1, float(abs(t[3]-t[2]))/t[3])

print(max1)
exit()#"""

all_theoretical_overlaps = []		# contains all subread data, from the overlaps

## all positions wrt original genome
## a1: startpos of read in ref.fa, a2: endpos of read in ref.fa
## b1: startpos of read in reads.fa, b2: endpos of read in reads.fa
## o1: startpos of overlap, o2: endpos of overlap
## sax: position of start end stop of subread, wrt the read
##		margin_right is larger, because the right is more flexible due to errors
max_length_subread = 0
margin_left = 200		# amount of bases with which the theoretical overlap is extended (if possible)
margin_right = 0.25		# observed max diff between original and erronous read length
for (idx1,r1) in enumerate(list1):
	for (idx2,r2) in enumerate(list2):
		a1 = r1[1]
		b1 = r2[1]
		a2 = a1 + r1[2]
		b2 = b1 + r2[2]
		if a2 < b1 or b2 < a1:
			continue
		o1 = max(a1, b1)
		o2 = min(a2, b2)
		ovl_length = o2 - o1
		#print("r1: %d, r2: %d, a1: %d, a2: %d, b1: %d, b2: %d, o1: %d, o2: %d, length: %d" % (idx1, idx2, a1, a2, b1, b2, o1, o2, ovl_length))
		"""sa1 = max(a1, o1 - margin_left) - a1
		sa2 = min(min(a2, o2 + int(margin_right*ovl_length)) - a1, list1[idx1][3])
		sb1 = max(b1, o1 - margin_left) - b1
		sb2 = min(min(b2, o2 + int(margin_right*ovl_length)) - b1, list2[idx2][3])
		"""
		sa1 = a1
		sa2 = a2
		sb1 = b1
		sb2 = b2
		if sb2 <= sb1:
			sb1 = sb2 - (sa2-sa1)
		if sa2 <= sa1:
			sa1 = sa2 - (sb2-sb1)
		max_length_subread = max(max_length_subread, sa2-sa1)
		max_length_subread = max(max_length_subread, sb2-sb1)
		#print("%d %d" % (sa2-sa1, sb2-sb1))
		if ovl_length > 1000:
			all_theoretical_overlaps.append([idx1, idx2, sa1, sa2, sb1, sb2])
		#print("r1: %d, r2: %d, sa1: %d, sa2: %d, sb1: %d, sb2: %d" % (idx1, idx2, sa1, sa2, sb1, sb2))
		#print()
print("Max length subread: " + str(max_length_subread))
print("Num theoretical ovls: %d" % len(all_theoretical_overlaps))

# generate subreads that should have overlaps
f1 = open('../NPBSS/ecoliGenome.fa.npbss_simulated_CLR_1.fa','r')
f2 = open('../NPBSS/ecoliGenome.fa.npbss_simulated_CLR_2.fa','r')
fout1 = open('../sw_sse2/read.fasta','w')
fout2 = open('../sw_sse2/ref.fasta','w')

all_reads1 = f1.readlines()
all_reads2 = f2.readlines()

for s in all_theoretical_overlaps:
	idx1 = s[0]
	idx2 = s[1]
	sa1 = s[2]
	sa2 = s[3]
	sb1 = s[4]
	sb2 = s[5]

	read1 = all_reads1[idx1*2+1]
	read2 = all_reads2[idx2*2+1]

	"""subread1 = read1[sa1:sa2+1]
	subread2 = read2[sb1:sb2+1]
	"""
	subread1 = read1
	subread2 = read2

	fout1.write('>subread_%d_%d\n' % (idx1, sa1))	
	l = list((subread1[0+i:70+i] for i in range(0, len(subread1), 70)))
	for chunk in l:
		fout1.write(chunk)
		fout1.write('\n')
	fout2.write('>subread_%d_%d\n' % (idx2, sb1))	
	l = list((subread2[0+i:70+i] for i in range(0, len(subread2), 70)))
	for chunk in l:
		fout2.write(chunk)
		fout2.write('\n')

f1.close()
f2.close()
fout1.close()
fout2.close()

# run sw_sse2/ksw to find perfect overlaps and scores
print('Finding perfect overlaps')
cmd = './ksw -o -s -p -t 6000 -a 5 -b 4 -q 10 -r 1 read.fasta ref.fasta > perfect_overlaps'
#cmd = './ksw -o -s -p -t 1600 -a 1 -b 1 -q 1 -r 1 read.fasta ref.fasta > perfect_overlaps'
print(cmd)
subprocess.call(cmd, shell=True, cwd='../sw_sse2/')

# read perfect overlaps
## read_id, sa1, ref_id, sb1, ref_start, ref_end, read_start, read_end, score
print('Reading perfect overlaps')
all_perfect_overlaps = []
f1 = open('../sw_sse2/perfect_overlaps','r')
for ovl in f1:
	l = parse(ovl)
	if len(l) != 9:
		print('WARNING this perfect overlap does not have enough information')
		print(ovl)
		print(l)
	all_perfect_overlaps.append(l)

f1.close()

# analyze reported overlaps by heuristic aligner
## ref_id, gen_pos, org_length, act_length, \
## read_id, gen_pos, org_length, act_length, \
## ref_start, ref_end, read_start, read_end, score, comp, 0
## last 0 is used to find False Positives (FP)
print('Reading darwin overlaps')
f1 = open('out.darwin')
all_heuristic_overlaps = []
for line in f1:
	l = parse(line)
	if len(l) != 14:
		print('WARNING this darwin overlap does not have enough information')
		print(line)
		print(l)
	l.append(0)
	all_heuristic_overlaps.append(l)

f1.close()



# compare perfect overlaps and heuristic overlaps
n = 0
## add sa1 and sb1 to the report perfect positions in terms of genome coordinates
## perfect_overlap after:
## read_id, ref_id, ref_start, ref_end, read_start, read_end, score
for povl in all_perfect_overlaps:
	povl[4] += povl[1]
	povl[5] += povl[1]
	povl[6] += povl[3]
	povl[7] += povl[3]
	povl.pop(3)
	povl.pop(1)

## compare perfect and heuristic overlaps
same_score = 0
higher_score = 0		# how many heuristic overlaps have a higher score
lower_score = 0
c1 = 0				# higher score, diff < 50
c2 = 0				# higher score, diff < 200
c3 = 0				# lower score, diff < 20
FN = 0				# false negatives, a povl has no matching hovl, thus heuristic missed one
FP = 0				# false positives, a hovl has no matching povl, thus should not exist
for povl in all_perfect_overlaps:
	fn = 1
	for hovl in all_heuristic_overlaps:
		if povl[0] == hovl[0] and povl[1] == hovl[4]:
			fn = 0
			hovl[13] = 1
			#print povl
			# remove some info from darwin overlap
			tovl = [y for x in [[hovl[0]], [hovl[4]], hovl[8:]] for y in x]
			#print tovl
			#print()

			n = n + 1
			if hovl[12] == povl[6]:
				same_score = same_score + 1
			elif hovl[12] > povl[6]:
				higher_score = higher_score + 1
				if hovl[12] - povl[6] < 50:
					c1 = c1 + 1
				if hovl[12] - povl[6] < 200:
					c2 = c2 + 1
			elif hovl[12] < povl[6]:
				lower_score = lower_score + 1
				if povl[6] - hovl[12] < 20:
					c3 = c3 + 1
			#if n % 10 == 0:
				#print("ref_id, read_id, ref_start, ref_end, read_start, read_end, score")
	if fn == 1:
		FN = FN + 1
		#print povl

for hovl in all_heuristic_overlaps:
	if hovl[13] == 0:
		FP = FP + 1

print("num perfect ovls: %d" % len(all_perfect_overlaps))
print("n: %d" % n)
print("same score: %d" % same_score)
print("higher score: %d" % higher_score)
print("lower score: %d" % lower_score)

print("c1: %d" % c1)
print("c2: %d" % c2)
print("c3: %d" % c3)

print("FN: %d" % FN)
print("FP: %d" % FP)

print("sensitivity: %f" % (float(n)/(n+FN)))
print("specificity: %f" % (float(n)/(n+FP)))
