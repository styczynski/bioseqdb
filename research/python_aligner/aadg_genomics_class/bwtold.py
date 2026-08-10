"""Burrows-Wheeler Alignment - String Search
--------------------------------------------
This is a very simply quick and dirty implementation of a Burrows-Wheeler Aligner for indexing and sequence alignment.
It does NOT implement any of the heuristic methods included in the actual BWA algorithm, so implements the basic algorithm which returns 100% accurate results
It uses the Burrows-Wheeler transform (BWT), Suffix Array (SA), and 2 other auxillary datastructures C and Occ (sometimes called O)
It uses the D array to prune the tree search and speed up the inexact search algorithm.
The search is case insensitive.

Differences between this code and the real BWA algorithm
--------------------------------------------------------
- This is NOT IN ANY WAY optimised. This has been coded for ease of understanding, ease of reading and with the goal of better understaning the basic BWA algorithm and its datastructures
- This code parses the suffix tree using a recursive depth-first search, while the real BWA uses breadth-first search (and a heap datastructure), and uses this to prioritise the best partial alignments first
- If BWA finds a result with a difference score of z, it only further considers matches with difference z+1, speeding up the process by ignoring worse results and pruning the search space more aggresively
- BWA sets a maximum allowed difference in the first few tens of bases (called the seed) resulting in 2.5x improvement, and very little loss in accuracy. This is not effective on shorter reads (<50bp)
- BWA reduces the required operating memory by storing small fractions of the Occ and SA arrays, and calculating the rest on the fly. This implementation does not do this.
"""

Mtarget="AGATCCTGTTCTGGTCAGCAGGGTGGTGACCAGAAAACAAGTTCTTCTGGTCTCCTACCTCAGTTGCAAGAAACTCAAAACTCCATTTTAAACTTTAAGCCTTATAGTATCTTCTGTTTGAGTATTTAACAAGCATTTGTTTTTCCTTGAAAATATATCCAGCCAAGACCTTATGAAGAATGTGAGCTAAAATTACGGTATTTTACTTGCCTGGAAACAGTACTTCTCAAATGTTAATGTGCATACAGGTGACTTGAGTATCTCATTAAAACGTAGATTTAGATTGTGTCTCCGGGGTGAGGTCTGAGAAACTTCATTTCTATAAAGTGATGTCAAAGTTACTGGTCTGGGGACCATGTTTTGGTGGCAAGTTGCAAGACCCCAGAGTTTCTACCCTAATGATTTATTCAATGACTCTTAGAGGTGTTATCAATCTGTTTTTAAAGCCAGGGACTTTGCCCAGGGGAAAAATGCATGCATACACACACACACACACACACACACACACACACACCCCTATGCGTACAATTTCGGAGCAATTGTGCATTCATTCAGTGAATACCTATTTAATGCACACCAAGTATCTTTCCGGGTGCTAATGACAGAGTGAGGAACAAGATAGCAAAGATGCCTGCCTTGTGGAGCTTTCATTATACTGTTGGTTGGAAGACAAACTAAGTAAATAAAGCAGGCATGTCAGCTATGATACATGCCTTAGGACAGTGTACTGCAGCCACGTGATACAGGGATTGGGTGGAGGAAGGTTTGAAGTGGCTCTTTTAAATTGTTCTGTCTGTGGAGGCATCTTGGCCAGGAACCTGAATGCCAGTATCTGGGGAAAAGCATCCTAGGTGGTAAGTTCGGAGCACCTGAAGCAGAAATGAGTTTAGTGTTTTCAAAAGTTAGAGAGAACTGGTATGGGAGGAACAGAGCGAGTGGAGGAGAGAGCAAAAGGTGAAATCCCAGAGGTAGAAGGGCCTGATCCTACAGGAGCTTGTAGGCCATGACAAGGAGGCTGCCTGCACTTGACCGAGCCCATGCTTAGATCCTAAAGGAGCCATGGCCTCCATTTAAGAACTCCAGACAGAGAATC"
Mquery="AGAAAACCAGTTCTTCTGGTCTCCTACCTCAGTTGCAAGAAACCCAAAACTCCATTTTAAACTTTCAGCCTTATAGTATCTTCTGTTTGAGTATTTAACAAGCAGTTGTTTTTCCTTGAAAATAACCAGCCAAGGCCTTATGACGAATGTCAGCTAAAATTACGGTATTTTACTTGCCTGGAAACAGTACTTCTCAAAAGTTAATGTGCATAGTGGTAACTTGAGTATCTATCATTAAAACGTAGATTTAGATTGTGTCTCCGGGGTGAGGTCGGAGAAACTTCATCTCTATAAAGTGATGTCAAAGTTACTGGTCTGGGGACCATGTTTTGGTGGCAAGTTGCAAGAGCCGAGAGTTGCTACCCTAATGATTTATTCAATGACTCTTAGAGGTGTTATCAATCTGTTTTTAAAGGCAGGGACTTTGCCCAGGGGAAAAACGCATGCATACACACACACACACACACACACACACACACACACCCCTATGCGGACAATTTAGGAGCAATTATGCATTCATTCAGTGAATACATATTTAATGCACACCAAGTATCTTTCCGTGTGCTAATGACAGAGTGAGGAACAGTATAGCAAAGATGCCTGCCTCGTGGAGATATCATTATAACTGTTGGTTGGAAGACAACTAAGTAAATAAAGCAGGCAAGTCAGCTATGATACATGCCTTAGGACAGTGTACTGCAGCCACGTGATTCAGGGATTGGGTGGAGGAAGGTTTGAAGTGGCTCTTTTAATGTTCTGTCTGTGGAGGCATCTTGGCCAGGAACCTGAATGCCGGTATCTGGGTAAAAGCATCCTAGGTGGTAAGTTCGGAGCACCTGAAGAGAAATGAGATTAGTGAATTCAAAAGTTAGAGAGCACTGGTATGGGAGGAACAGAGCTAGTGGAGGAGAGAGCAAAAGTGAAATCCCAGAGGTAGAAGGGCCTGATCCTACAGGAGCTTGTAGGCCATGACAAAGAGGCTGCCTGCACTTGACCGAGC"


__author__ = "Justin Womersley"
__date__ = "2011/02/15"
__license__ = "Python"
__version__ = "0.1"

class BWA:
	"""
	A Burrows-Wheeler Alignment class. Contains the 4 core datastructures used in the algorithm, SA, BWT, C and Occ which are created in the constructor which is passed the reference string as an argument
	"""
	#this initializer function creates all the datastructures necessary using the reference string
	def __init__(self, reference):
		#declare datastructures
		rotation_list, rotation_list_reverse, suffix_array, bwt = [list() for i in range(4)]
		C, Occ, Occ_reverse = [dict() for i in range(3)]
		alphabet = set()
		reverse_reference = reference[::-1]#reverse reference
		
		#Construct the alphabet. (This would be hard coded for DNA examples)
		reference = reference
		for char in reference:
			alphabet.add(char)
		
		#initialize 2 auxillary datastructures
		for char in alphabet:
			C[char] = 0
			Occ[char] = list()# in Occ, each character has an associated list of integer values (for each index along the reference)
			Occ_reverse[char] = list()
	
		#append the ending character to the reference string
		reference = "%s$" % reference
		reverse_reference = "%s$" % reverse_reference

		#create all the rotation/suffix combinations of the reference and reverse reference, and their starting index positions
		for i in range(len(reference)):
			new_rotation = "%s%s" % (reference[i:],reference[0:i])
			struct = Suffix(new_rotation,i)
			rotation_list.append(struct)
			
			new_rotation_reverse = "%s%s" % (reverse_reference[i:],reverse_reference[0:i])
			struct_rev = Suffix(new_rotation_reverse,i)
			rotation_list_reverse.append(struct_rev)
		
			#create the C datastructure. C(a) = the number of characters 'a' in the Reference that are lexographically smaller than 'a'
			#NOTE, the C datastructure is not required for the reverse reference
			if reference[i]!='$':
				for char in alphabet:
					if reference[i] < char:
						C[char] = C[char] + 1	
		
		#sort the rotations/suffixes using the suffix/rotation text as the key
		rotation_list.sort(key=textKey)
		rotation_list_reverse.sort(key=textKey)
	
		#now record the results into 2 seperate lists, the suffix (or S) array and the BWT (or B) array
		#also calculate the auxilliary datastructure Occ (or O)
		for i in rotation_list:
			suffix_array.append(i.pos)#the position of the reordered suffixes forms the Suffix Array elements
			bwt.append(i.text[-1:])#the last character in each rotation (in the new order) forms the BWT string elements
		
			#now construct the Occ (or C) datastructure
			for char in alphabet:
				if len(Occ[char]) == 0:
					prev = 0
				else:
					prev = Occ[char][-1]
				if i.text[-1:] == char:
					Occ[char].append(prev+1)
				else:
					Occ[char].append(prev)
					
		#now record the results into 2 seperate lists, the suffix (or S) array and the BWT (or B) array
		#also calculate the auxilliary datastructures, C and Occ (or O)
		for i in rotation_list_reverse:
			#construct the Occ (or C) datastructure
			for char in alphabet:
				if len(Occ_reverse[char]) == 0:
					prev = 0
				else:
					prev = Occ_reverse[char][-1]
				if i.text[-1:] == char:
					Occ_reverse[char].append(prev+1)
				else:
					Occ_reverse[char].append(prev)					
					
		#save all the useful datastructures as class variables for easy future access
		self.SA = suffix_array
		self.BWT = bwt
		self.C = C
		self.Occ = Occ
		self.Occ_reverse = Occ_reverse #the Occ datastructure for the reverse reference, using to construct the D array (the lower bound on the number of differences allowed), to speed up alignments 
		self.n = len(reference)
		self.D = list()#empty list for later use
		self.alphabet = alphabet

	#get the position(s) of the query in the reference
	def find_match(self,query,num_differences):
		if num_differences == 0:
			return self.exact_match(query)
		else:
			return self.inexact_match(query,num_differences)

	#exact matching - no indels or mismatches allowed
	def exact_match(self, query):
		query = query
		i = 0
		j = self.n - 1
		
		for x in range(len(query)):
			newChar = query[-x-1]
			newI = self.C[newChar] + self.OCC(newChar,i-1) + 1
			newJ = self.C[newChar] + self.OCC(newChar,j)
			i = newI
			j = newJ
		matches = self.SA[i:j+1]
		return matches

	#inexact matching, z is the max threshold for allowed edits
	def inexact_match(self,query,z):
		self.calculate_d(query)
		SA_indeces = self.inexact_recursion(query, len(query)-1, z, 0, self.n-1)
		return [self.SA[x] for x in SA_indeces]#return the values in the SA

	#recursion function that effectively "walks" through the suffix tree using the SA, BWT, Occ and C datastructures
	def inexact_recursion(self,query,i,z,k,l):
		tempset = set()
			
		#2 stop conditions, one when too many differences have been encountered, another when the entire query has been matched, terminating in success
		if (z < self.get_D(i) and use_lower_bound_tree_pruning) or (z < 0 and not use_lower_bound_tree_pruning):#reached the limit of differences at this stage, terminate this path traversal
			return set()#return empty set	
		if i < 0:#empty query string, entire query has been matched, return SA indexes k:l
			for m in range(k,l+1):
				tempset.add(m)
			return tempset
			
		result = set()
		if indels_allowed: result = result.union(self.inexact_recursion(query,i-1,z-insertion_penalty,k,l))#without finding a match or altering k or l, move on down the query string. Insertion
		for char in self.alphabet:#for each character in the alphabet
			#find the SA interval for the char
			newK = self.C[char] + self.OCC(char,k-1) + 1 
			newL = self.C[char] + self.OCC(char,l)
			if newK <= newL:#if the substring was found
				if indels_allowed: result = result.union(self.inexact_recursion(query,i,z-deletion_penalty,newK,newL))# Deletion
				if char == query[i]:#if the char was correctly aligned, then continue without decrementing z (differences)
					result = result.union(self.inexact_recursion(query,i-1,z,newK,newL))
				else:#continue but decrement z, to indicate that this was a difference/unalignment
					result = result.union(self.inexact_recursion(query,i-1,z-mismatch_penalty,newK,newL))
		return result

	#calculates the D array for a query, used to prune the tree walk and increase speed for inexact searching
	def calculate_d(self,query):
		k = 0
		l = self.n-1
		z = 0
		self.D = list()#empty the D array
		for i in range(len(query)):
			k = self.C[query[i]] + self.OCC(query[i],k-1,reverse=True) + 1
			l = self.C[query[i]] + self.OCC(query[i],l,reverse=True)
			if k > l:#if this character has NOT been found
				k = 0
				l = self.n - 1
				z = z + 1
			self.D.append(z)

	#returns normal Occ value, otherwise returns the reverse Occ if explicitly passed as an argument
	#NOTE Occ('a',-1) = 0 for all 'a'
	def OCC(self,char,index,reverse=False):
		if index < 0:
			return 0
		else:
			if reverse:
				return self.Occ_reverse[char][index]
			else:
				return self.Occ[char][index]
	
	#gets values from the D array
	#NOTE D(-1) = 0
	def get_D(self,index):
		if index < 0:
			return 0
		else:
			return self.D[index]

class Suffix:
	"""
	A simple class with 2 variables, used for sorting and calculating the Suffix Array and BWT array
	Each instance holds the position of the suffix and the suffix (text) itself
	"""
	def __init__(self, text, position):
		self.text = text
		self.pos = position

#this is used to sort the Suffix objects, according to their text key
def textKey( a ): return a.text

#environment variables
debug = True
show_data_structures = True
use_lower_bound_tree_pruning = True #set this to false (in conjunction with debug=True) to see the full search through the suffix trie
#search parameters
indels_allowed = True # turn off for mismatches only, no insertion or deletions allowed
difference_threshold = 4
insertion_penalty = 1
deletion_penalty = 1
mismatch_penalty = 1

#reference and query strings
reference = Mtarget[::-1][:200]
query = Mquery[::-1][:100]

if __name__ == "__main__":
	#index the reference and build up all the necessary datastructures (5 of them; BWT, SA, reference alphabet, C and OCC arrays)
	data = BWA(reference)
	print(data.C)
	matches = data.find_match(query,difference_threshold)
	print(f"{len(matches)} match(es) at position(s): {matches}")