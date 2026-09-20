"""Remote native DNA5 encoding regression; excluded from matrix timings."""
from pathlib import Path
import platform
import random
import subprocess
import sys
import tempfile

if platform.system()!='Linux':raise RuntimeError('Remote Linux builder required')
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'bench/e2e_external'))
from campaign import validate, reverse_complement
from adapter import setup, align, parse

rng=random.Random(74337)
reference=''.join(rng.choices('ACGNT',weights=[.2475,.2475,.2475,.01,.2475],k=80000))
queries={'q0':reference[10000:30000],'q1':reverse_complement(reference[45000:65000])}
truth=[{'start':start,'end':end,'reverse':reverse,'window_start':0,'window':reference}
       for start,end,reverse in [(10000,30000,False),(45000,65000,True)]]

def run(argv):
    completed=subprocess.run(list(map(str,argv)),capture_output=True,text=True,timeout=90)
    if completed.returncode:raise RuntimeError(completed.stderr[-4000:])

with tempfile.TemporaryDirectory(prefix='lexicmap-reference-encoding-') as temporary:
    work=Path(temporary);fixture=work/'fixture';fixture.mkdir();index=work/'index';index.mkdir()
    (fixture/'target.fasta').write_text('>original\n'+reference+'\n')
    query=fixture/'queries.fasta'
    query.write_text(''.join('>'+name+'\n'+sequence+'\n' for name,sequence in queries.items()))
    config={'data':{'kmer_size':15,'window_size':10,'alphabet':'dna5'},'threads':4}
    setup(ROOT,config,fixture,index,run)
    output=align(ROOT,config,fixture,index,query,work,run)
    rows=list(parse(output));audit=validate(rows,truth,queries,2,False)
    assert audit['native_encoded_reference_bases_checked']>200,audit
    assert all('N' in row['qaln'] and 'N' not in row['taln'] for row in rows),rows
    print('LexicMap native DNA5 replay: both strands, exact query bases, native N-to-A reference encoding, positions verified')
