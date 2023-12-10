from collections import defaultdict
import sys
import typing
from typing import List, Union
import random
import copy
from tqdm import tqdm
import os
from difflib import ndiff

from .structures import *


def kmers(string:str, 
		k:int) -> str:

    for i in range(len(string) - k + 1):
        yield string[i:i+k]


def Tour(start:str, 
		graph:defaultdict, 
		end: Union[str, None] = None) -> List[str]:
    tour = [start] #

    if end is None:
        finish = start
    else:
        finish = end
    while True:
        if tour[-1] not in graph.keys():
            break

        nodes = graph[tour[-1]]
        if not nodes: # case sequence of nodes is a tour, not eulerian circle
            break
            
        tour.append(nodes.pop())
        if tour[-1] == finish: # case eulerian circle
            break

    offset = 0
    for i,step in enumerate(tour):
        try:
            nodes = graph[step]
            #print(nodes)
            if nodes: 
                tour_ = Tour(nodes.pop(), graph, step) 
                i += offset
                tour = tour[ : i + 1 ] + tour_ + tour[ i + 1 : ]
                offset += len(tour_)
                print("-----------", file=sys.stderr)
        except:
            continue
            
    return tour



def BuildDeBrujinGraph(reads: FastqExperiment,
	k:int = 12) -> defaultdict:
    """
    Function builds de Bruijn graph from all reads
    Takes a FastqExperiment object with reads
    Returns graph written as a dictionary
    """
    
    graph = defaultdict(list)
    all_kmers = set()
    for _,read in reads.items():
        for kmer in kmers(read.seq, k):
            head = kmer[:-1]
            tail = kmer[1:]
            graph[head].append(tail)

    return graph


def BuildGenomeFromDeBrujnGraph(graph:defaultdict) -> Genome:
    """
    Function builds genome from de Bruijn graph 
    Takes graph written as a dictionary
    Returns genome sequence
    """
    
    start = None
    for key in list(graph.keys()):
        if len(graph[key]) % 2 == 0 and len(graph[key]) != 0:
            start = key
            break
    print(f"START: {start}", file=sys.stderr)
    tour = Tour(start, graph)
    return Genome("".join([tour[0]] + [s[-1] for s in tour[1:]]))


def BuildGenome(reads: FastqExperiment, 
	k: int)-> Genome:
    """
    Function collects genome from all reads
    Takes a FastqExperiment object with reads
    Returns genome sequence
    """

    graph = BuildDeBrujinGraph(reads,k)
    genome = BuildGenomeFromDeBrujnGraph(graph)
    return genome

