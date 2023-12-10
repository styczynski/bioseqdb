import numpy as np
import random
import string
import os
from typing import List, Union, Tuple
import numpy as np

from .structures import *
from .assembler import *
def get_random_str(main_str:str,
 substr_len:int) -> Tuple[str, int]:
    idx = np.random.randint(0, len(main_str) - substr_len + 1)    # Randomly select an "idx" such that "idx + substr_len <= len(main_str)".
    return (main_str[idx : (idx+substr_len)], idx)



def generateGenome(length:int = 10000, 
    alphabet:List[str]=['a', 'c', 'g', 't'], 
    probs:Union[List[float], None] = None) -> Genome:
    if probs == None:
        probs = [1/len(alphabet)] * len(alphabet)
    else:
        assert len(alphabet) == len(probs), "Provide probabilities for each letter in alphabet"
    return Genome("".join(np.random.choice(alphabet, size=length, p=probs)))
 


def makeFastqFile(genome,
                  n_reads=1000,
                  read_length=100,
                  base_path:str = "", 
                  p_indel:float = 0.01, 
                  p_mut:float = 0.01, 
                  gap_length=10, 
                  debug=False) -> None:
    result = ""
    if debug:
        sampling_info = dict()
    for i in range(n_reads):
        idx = 0
        insert_occurance = np.random.choice([0, 1], p=[1-p_indel, p_indel])
        del_occurance = np.random.choice([0, 1], p=[1-p_indel, p_indel])
        if not del_occurance and not insert_occurance:
            read, idx = get_random_str(genome.data, read_length)
        else:
            if del_occurance: 
                read, idx = get_random_str(genome.data, read_length + gap_length)
                start = np.random.randint(0, read_length + 1)
                read = read[:start] + read[start+gap_length:]
                assert len(read) == read_length
                
                
            if insert_occurance: 
                read, idx = get_random_str(genome.data, read_length - gap_length)
                start = np.random.randint(0, read_length + 1)
                read = read[:start] + generateGenome(gap_length).data + read[start:]
        assert len(read) == read_length
        score = "".join(np.random.choice(list(string.printable[:94]), size=read_length))
        result += f"@SEQ_{i}\n"\
                + read + "\n"\
                + "+\n"\
                + score + "\n"
        if debug:
            sampling_info[f"SEQ_{i}"] = [idx]
    with open(os.path.join(base_path, "experiment.fastq"), "w") as fastq_file:
        fastq_file.write(result)
        
    if debug:
        return sampling_info


def yieldOverlappingSubstrings(text:str, 
                               l:int,
                              overlap: int):
    for i in range(0, len(text),l-overlap ):
        if i + l < len(text) - 1:
            yield text[i:i+l]
        else:
            yield text[-l:]
            break


def ShotgunGenome(genome: Genome,
                 read_length: int = 100, 
                 overlap:int = 15,
                 n_resamples:int=200) -> FastqExperiment:
    res = FastqExperiment()
    for i, seq in enumerate(yieldOverlappingSubstrings(genome.data, read_length, overlap)):
        read = FastqRead(seq=seq, 
                         name= f"SEQ_{i}", 
                         quality="".join(np.random.choice(list(string.printable[:94]), size=read_length)))
        
        res[f"SEQ_{i}"] = read

    for j in range(n_resamples):
        res[f"SEQ_resampled_{j}"] = FastqRead(seq=get_random_str(genome.data, read_length)[0], 
            name = f"SEQ_resampled_{j}",
            quality="".join(np.random.choice(list(string.printable[:94]), 
                    size=read_length)))
    
    return res





def levenshtein_distance(str1, str2 ):
    counter = {"+": 0, "-": 0}
    distance = 0
    for edit_code, *_ in ndiff(str1, str2):
        if edit_code == " ":
            distance += max(counter.values())
            counter = {"+": 0, "-": 0}
        else: 
            counter[edit_code] += 1
    distance += max(counter.values())
    return distance


def test_assembly(genome_length_range:Tuple[int, int],
         n_iter:int=30,
        read_length:int = 100, 
        overlap: int = 15,
        k:int=12) -> Dict[int, float]:
    test_res = dict()
    exps_len = 0
    for genome_length in range(genome_length_range[0], genome_length_range[1] + 1, 5):
        print(f"ON LENGTH: {genome_length}")
        tmp_res_1 = []
        tmp_res_2 = 0
        
        for _ in range(n_iter):
            genome = generateGenome(length=genome_length)
            experiment = ShotgunGenome(genome, read_length=read_length, overlap=overlap, n_resamples=n_iter*20)
            #print(experiment.reads)
            genome_generated = BuildGenome(experiment, k=k)
            dist = levenshtein_distance(genome.data, genome_generated.data)
            tmp_res_1.append(dist)
            for _, read in experiment: 
                if read.seq in genome_generated:
                    tmp_res_2 += 1
            exps_len += len(experiment)
        
        test_res[genome_length] = (np.mean(tmp_res_1), tmp_res_2/exps_len)
        exps_len = 0
        
    return test_res
        