target = "CACTTCTAAGCTTAATCACTTTACATCTATACAATCATTCGCTATTTTTTAATCCTATGAAAAATGCAAATGTGTCCATAATCTCAACTCTGTAATTATAGCTCTGCAGGTTAACTGCTCTGAGTCTGAGTTTCCTCATTTGAAAAACTGGGGTACTAATAACACCTATTTCACATAGATGATGTGAAAATTAAATAAGCAATAAGTAGAAAATTAGCTGTTATCCAGAGGAAGATCAACAAATGATGGCTGTTGTTAAAAAAATAAAAAGCAAAGAGAGTAATTCTTTTTTTAATAAAGCTTTAATGAGGCTTAAGTGACCTACTATGTGTAAGAGGCTTTGCACCGAGTAGGAACTCAACAACATTTAATTCCTGCACTCCCTTTCTTCCTTCTTTGCTTAGTGAAAAAAAGAAAGTTGATGGACACACTCATTGTGGATAATTTATACTATTTTCTGAAGCAGAGTAGACTCAGTCATTTCTTTTATCCAGTGCTAGTAATAATGTTTGGCAGAAAAAGTCTCACTCAAAAATGAAAGAGGTAACTGACTCAACTAAAGACGTGGCATGCAAAATCCTGTTGGTGTCAATCCAGCTAACATTTCTCCCCCAATGGTGGATGCCTAGAATTTCACAGTCTCTTCTCTACTTTTGATTTTGGTACCTCTTAAACCATTACTGCTCCTACAAATACTTCTGAGTAAGTATCCATACCTTCCCTGGAGTATGAGAGTGAAATAGATTTATAAGTAGATTTTTAGAATTAATGTTTAGGGATTTGCATGAGGGAAGGAAGAGAGGAGGAAAGGAATGCTTGTGGCTGCTGAATTTCAGAAGGAAATGGAAGTAGAATCTACAAAAAGGGAAGGGAAACATATTGAAGACAAAAAAACAGTTTCCTACTGCATAAATTTTCCTGGATTTTATCCTTAATCCGAATGCATAAGGAACATCTACCATGCATTTGAGATGTCTGAGTGGGTCTCATCCAAGGGAAAGAAGCATGTACTTTCTTAATGGTTTCTTTGTTGCTACTTTAGCCTTACAAACTGGTGACTCAAAAATGGACTGCTCTAGCTCTTCAACTCTTTTCTTATCTAGCTAAAGATTTTAGGGTAT"
#query = "ATCCTATGAAAAATGCAAATGTGTCCATAAGCACAACTCTGTAATTATAGCTCTGCAGGTTAACTGATCTGAGTCTGAGTTTCCTCATTTGAATAACTGGGGTACTAATAACACCTATTTCACATAGATGATGTGAATATTCTATAAGCAATAAGTAGAAAATTAGCTGTTATACATAGGAAGATGAACAAATGATGGCTGTTGTTAAAAAAATAAAAAGCAAAGAGAGTAATTCTTTTTTTAAAAAGCTTTAATGAGGATTAAGTGACCTACTATCTGCAAGAGGCTTTGCACCGAGTAGGAACTCAGCAACATTTATTTCCTGCACTCCCTTTCTTCCTTCTTTGCTTTGTGAAAAAAAGACAAGCTGATGGACACACTCATTAGGGATAATTTATACTATTTTCTGAAGCAGAGTAGACTCAGTCATTTCTTTTATCCAGTTGCTAGTAATAGATGTTTGGCAGAAAAAGTCTCACTCAATGATGCAAGAGGTAACT"
query_end = "GACTCACTAAAGACGTGGCATGCAAAATCCCGTTGGTGTCCATCCAGTTACATTTCTCCCCCAATGGTGGATGCCTAGAATTTCACAGTCTCTTCTCTACTTTTGATTTTGGTACCTCTTAAACCATTACTGCTCCTACAAATTCGTCTGAGTAATTATCCATACCTTCCCTGGAGTATGAGAGTGAAATAGATCTATAAGTAGATTTTTAGAATTAATGCTTAGGATTTGCATGAGGGAAGGAAGAGAGGAGGAAAGGAATGCTTGTGGCTGCTGATTTCAGAAGGAAATGGAAGTAGAATCTACAAACAGGGAAGGGAAACATATTGAAGACAAAAAAACAGTTTCCTACTGCATGAATTTTCCTGGATTTAATCCTTAATCCGAATGCATCAGGAACATCTACCATGCATTTGAGATGTCTGAGTGGGTCCAATCCAAGGGAAAGAAGCAAGTACTTTCTTAATGGTTTCTTTGTTGCTACATTAGCATTCCAAACT"

import sys
import pdb
import numpy as np
import copy

score_insertion    = -20
score_deletion     = -20
score_substitution = -1
score_match        =  8

def seq_align(x, y):
  """
  input:  two strings: x and y
  output: an array with a length of |y| that contains the score for the alignment 
          between x and y
  """
  global score_insertion
  global score_deletion
  global score_substitution
  global score_match 
  row = y
  column = x 
  minLen = len(y)
  prev = [0 for i in range(minLen + 1)]
  current = [0 for i in range(minLen + 1)]


  for i in range(1, minLen + 1):
    prev[i] = prev[i-1] + score_insertion
  
  current[0] = 0
  for j in range(1, len(column) + 1):
    current[0] += score_deletion
    for i in range(1, minLen + 1):
      if row[i-1] == column[j-1]:
        try:
          current[i] = max(current[i-1] + score_insertion, prev[i-1] + score_match, prev[i] + score_deletion)
        except:
          pdb.set_trace()
      else:
        current[i] = max(current[i-1] + score_insertion, prev[i-1] + score_substitution, prev[i] + score_deletion)
    prev = copy.deepcopy(current) # for python its very import to use deepcopy here

  return current 

def partition_nw_scores(scoreL, scoreR):
  max_index = 0
  max_sum = float('-Inf')
  for i, (l, r) in enumerate(zip(scoreL, scoreR[::-1])):
    # calculate the diagonal maximum index
    if sum([l, r]) > max_sum:
      max_sum = sum([l, r])
      max_index = i
  return max_index 

def run_nw(x, y):
  global score_insertion
  global score_deletion
  global score_substitution
  global score_match 
  # M records is the score array
  # Path stores the path information, inside of Path:
  # d denotes: diagnal
  # u denotes: up
  # l denotes: left
  M = np.zeros((len(x) + 1, len(y) + 1))
  Path = np.empty((len(x) + 1, len(y) + 1), dtype=object)

  for i in range(1, len(y) + 1):
    M[0][i] = M[0][i-1] + score_insertion
    Path[0][i] = "l"
  for j in range(1, len(x) + 1):
    M[j][0] = M[j-1][0] + score_deletion
    Path[j][0] = "u"
  
  for i in range(1, len(x) + 1):
    for j in range(1, len(y) + 1):
      if x[i-1] == y[j-1]:
        M[i][j] = max(M[i-1][j-1] + score_match, M[i-1][j] + score_insertion, M[i][j-1] + score_deletion)
        if M[i][j] == M[i-1][j-1] + score_match:
          Path[i][j] =  "d"
        elif M[i][j] == M[i-1][j] + score_insertion:
          Path[i][j] = "u"
        else:
          Path[i][j] = "l"
      else:
        M[i][j] = max(M[i-1][j-1] + score_substitution, M[i-1][j] + score_insertion, M[i][j-1] + score_deletion)
        if M[i][j] == M[i-1][j-1] + score_substitution:
          Path[i][j] =  "d"
        elif M[i][j] == M[i-1][j] + score_insertion:
          Path[i][j] = "u"
        else:
          Path[i][j] = "l"

  pad_left, pad_right = 0, 0
  pad_right_act = True
  i = len(x)
  j = len(y)
  while Path[i][j]:
    if Path[i][j] == "d":
      pad_left = 0
      pad_right_act = False
      i -= 1
      j -= 1
    elif Path[i][j] == "u":
      pad_left += 1
      if pad_right_act:
        pad_right += 1
      i -= 1
    elif Path[i][j] == "l":
      pad_left = 0
      pad_right_act = False
      j -= 1
  return pad_left, pad_right


def run_hirschberge(x, y):
  pad_left = 0
#  x is being row-wise iterated (out-most for loop)
#  y is being column-wise iterated (inner-most of the for loop)
  if len(x) == 0 or len(y) == 0:
    if len(x) == 0:
      pad_left, pad_right = 0, 0
    else:
      pad_left, pad_right = len(x), len(x)
  elif len(x) == 1 or len(y) == 1:
    pad_left, pad_right = run_nw(x, y)
    # concatenate into string
    #row = "".join(row)
  else:
    xlen = len(x)
    xmid = xlen//2
    ylen = len(y)

    scoreL = seq_align(x[:xmid], y)
    scoreR = seq_align(x[xmid:][::-1], y[::-1])
    ymid = partition_nw_scores(scoreL, scoreR)
    pad_left_l, pad_right_l = run_hirschberge(x[:xmid], y[:ymid])
    pad_left_r, pad_right_r = run_hirschberge(x[xmid:], y[ymid:])

    pad_left = pad_left_l if pad_left_l < len(x[:xmid]) else pad_left_r + pad_left_l
    pad_right = pad_right_r if pad_right_l < len(x[:xmid]) else pad_right_r + pad_right_l

  return pad_left, pad_right
        

if __name__ == '__main__':
  seqstr1 = target[::-1]
  seqstr2 = query_end[::-1]
  for i, (x, y) in enumerate(zip([seqstr1], [seqstr2])):
    l, r = run_hirschberge(x, y)
    print(l)
    print(r)
