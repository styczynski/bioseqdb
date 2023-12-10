from .structures import *
import datetime
from tqdm import tqdm
import sys
from typing import Union
from Bio.Seq import Seq

from aadg_genomics_class.monitoring.logs import LOGS

class BWAligner(object):
#this initializer function creates all the datastructures necessary using the reference string
    def __init__(self, 
                genome:Genome, 
                indels_allowed:bool = True,
                insertion_penalty:int = 1,
                deletion_penalty:int = 1,
                mismatch_penalty:int = 1,
                use_lower_bound_tree_pruning:bool = True,
                debug=False):

        self.indels_allowed = indels_allowed
        self.insertion_penalty = insertion_penalty
        self.deletion_penalty = deletion_penalty
        self.mismatch_penalty = mismatch_penalty
        self.use_lower_bound_tree_pruning = use_lower_bound_tree_pruning
        self.debug = debug
        reference = genome
        #declare datastructures
        rotation_list, rotation_list_reverse, bwt = list(), list(), list()
        self.suffix_array = list()
        self.n = len(reference)
        self.C = dict()
        self.Occ = dict()
        self.Occ_reverse = dict()
        self.alphabet = {'a', 'c', 'g', 't'}
        self.D = list() #empty list for later use
        
        reverse_reference = reference[::-1] #reverse reference
        
        LOGS.aligner.info("Setting up indices")
        #Construct the alphabet. (This would be hard coded for DNA examples)
        #initialize 2 auxillary datastructures
        for char in self.alphabet:
            self.C[char] = 0
            self.Occ[char] = list() # in Occ, each character has an associated list of integer values (for each index along the reference)
            self.Occ_reverse[char] = list()
    
        #append the ending character to the reference string
        reference += "$"
        reverse_reference += "$"

        #create all the rotation/suffix combinations of the reference and reverse reference, and their starting index positions
        for i in range(len(reference)):
            if i % 1000 == 0:
                LOGS.aligner.info(f"Setting up indices for position in ref: {i}")
            # TODO: FIX!
            #new_rotation = Seq("".join((str(reference[i:]),str(reference[0:i]))))
            new_rotation = "".join((reference[i:],reference[0:i]))
            struct = Suffix(new_rotation,i)
            rotation_list.append(struct)
            
            # TODO: FIX!
            new_rotation_reverse = "".join((reverse_reference[i:],reverse_reference[0:i]))
            #new_rotation_reverse = Seq("".join((str(reverse_reference[i:]),str(reverse_reference[0:i]))))
            struct_rev = Suffix(new_rotation_reverse,i)
            rotation_list_reverse.append(struct_rev)
        
            #create the C datastructure. C(a) = the number of characters 'a' in the Reference that are lexographically smaller than 'a'
            #NOTE, the C datastructure is not required for the reverse reference
            if reference[i] != '$':
                for char in self.alphabet:
                    if reference[i] < char:
                        self.C[char] += 1

        #sort the rotations/suffixes using the suffix/rotation text as the key
        rotation_list.sort(key=lambda item: item.text)
        rotation_list_reverse.sort(key=lambda s: s.text)

        #now record the results into 2 seperate lists, the suffix (or S) array and the BWT (or B) array
        #also calculate the auxilliary datastructure Occ (or O)
        for i in rotation_list:
            if i % 1000 == 0:
                LOGS.aligner.info(f"Setting up indices for position in ref: {i}")
            self.suffix_array.append(i.position) #the position of the reordered suffixes forms the Suffix Array elements
            bwt.append(i.text[-1])#the last character in each rotation (in the new order) forms the BWT string elements
        
            #now construct the Occ (or C) datastructure
            for char in self.alphabet:
                if len(self.Occ[char]) == 0:
                    prev = 0
                else:
                    prev = self.Occ[char][-1]
                if i.text[-1:] == char:
                    self.Occ[char].append(prev+1)
                else:
                    self.Occ[char].append(prev)
                    
        #now record the results into 2 seperate lists, the suffix (or S) array and the BWT (or B) array
        #also calculate the auxilliary datastructures, C and Occ (or O)
        for i in rotation_list_reverse:
            if i % 1000 == 0:
                LOGS.aligner.info(f"Setting up indices for position in ref: {i}")
            #construct the Occ (or C) datastructure
            for char in self.alphabet:
                if len(self.Occ_reverse[char]) == 0:
                    prev = 0
                else:
                    prev = self.Occ_reverse[char][-1]
                if i.text[-1:] == char:
                    self.Occ_reverse[char].append(prev+1)
                else:
                    self.Occ_reverse[char].append(prev)
                    
                    
         #the Occ datastructure for the reverse reference, using to construct the D array (the lower bound on the number of differences allowed), to speed up alignments 
        LOGS.aligner.info(f"Finished aligner indices setup")
        
        
    def align(self, 
        reads:FastqExperiment, 
        difference_threshold:Union[int, None] = None)->AlignmentResult:
        """
        """
        LOGS.aligner.info("Performing the alignment")

        res = AlignmentResult(start=datetime.datetime.strftime(datetime.datetime.now(), "%Y-%m-%d %H:%M:%S"))
       
        for read_name, read in reads.items():
            LOGS.aligner.info(f"Aligning sequence: '{read_name}'")
            continue
            if difference_threshold == None:
                read_position = self._find_match(read.seq, len(read)// 20)
            else: 
                read_position = self._find_match(read.seq, difference_threshold)

            res[read_name] = read_position

        res.stop = datetime.datetime.strftime(datetime.datetime.now(), "%Y-%m-%d %H:%M:%S")
        LOGS.aligner.info("Alignment was completed")

        return res
            

    #get the position(s) of the query in the reference
    def _find_match(self,
        query:str,
        difference_threshold:int):
        if difference_threshold == 0:
            return self._exact_match(query)
        else:
            return self._inexact_match(query,difference_threshold)

    #exact matching - no indels or mismatches allowed
    def _exact_match(self, 
        query:str):
        query = query.lower()
        i = 0
        j = self.n - 1
        
        for x in range(len(query)):
            newChar = query[-x-1]
            newI = self.C[newChar] + self.OCC(newChar,i-1) + 1
            newJ = self.C[newChar] + self.OCC(newChar,j)
            i = newI
            j = newJ
        matches = self.suffix_array[i:j+1]
        return matches

    #inexact matching, z is the max threshold for allowed edits
    def _inexact_match(self,
        query:str,
        z):

        self.calculate_d(query)
        self.suffix_array_indeces = self._inexact_recursion(query, len(query)-1, z, 0, self.n-1)
        return [self.suffix_array[x] for x in self.suffix_array_indeces]#return the values in the SA

    #recursion function that effectively "walks" through the suffix tree using the SA, BWT, Occ and C datastructures
    def _inexact_recursion(self,
        query:str,
        i,
        z,
        k,
        l):
        tempset = set()
            
        #2 stop conditions, one when too many differences have been encountered, another when the entire query has been matched, terminating in success
        if (z < self.get_D(i) and self.use_lower_bound_tree_pruning) or (z < 0 and not self.use_lower_bound_tree_pruning): #reached the limit of differences at this stage, terminate this path traversal
            if self.debug:
                print ("too many differences, terminating path\n", file=sys.stderr)
            return set()#return empty set	
        if i < 0:#empty query string, entire query has been matched, return SA indexes k:l
            if self.debug:
                print (f"query string finished, terminating path, success! k = {k}, l = {l} \n", file=sys.stderr)
            for m in range(k,l+1):
                tempset.add(m)
            return tempset
            
        result = set()
        if self.indels_allowed: 
            result = result.union(self._inexact_recursion(query,i-1,z-self.insertion_penalty,k,l))#without finding a match or altering k or l, move on down the query string. Insertion
        for char in self.alphabet:#for each character in the alphabet
            #find the SA interval for the char
            newK = self.C[char] + self.OCC(char,k-1) + 1 
            newL = self.C[char] + self.OCC(char,l)
            if newK <= newL:#if the substring was found
                if self.indels_allowed: 
                    result = result.union(self._inexact_recursion(query,i,z-self.deletion_penalty,newK,newL))# Deletion
                if self.debug:
                    print( f"char '{char} found' with k = {newK} , l = {newL} , z = {z}: parent k = {k}, l = {l}", file=sys.stderr)
                if char == query[i]:#if the char was correctly aligned, then continue without decrementing z (differences)
                    result = result.union(self._inexact_recursion(query,i-1,z,newK,newL))
                else:#continue but decrement z, to indicate that this was a difference/unalignment
                    result = result.union(self._inexact_recursion(query,i-1,z-self.mismatch_penalty,newK,newL))
        return result

    #calculates the D array for a query, used to prune the tree walk and increase speed for inexact searching

    def calculate_d(self,
        query:str):
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
    def OCC(self,
        char:str,
        index:int,
        reverse:bool = False):
        if index < 0:
            return 0
        else:
            if reverse:
                return self.Occ_reverse[char][index]
            else:
                return self.Occ[char][index]

    #gets values from the D array
    #NOTE D(-1) = 0
        
    def get_D(self,
        index:int):
        if index < 0:
            return 0
        else:
            return self.D[index]
    

