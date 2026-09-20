"""Remote-only native index persistence parity test; not a matrix timing."""
from pathlib import Path
import platform
import random
import subprocess
import tempfile
from adapter import partition_reference, parse

if platform.system()!='Linux':raise RuntimeError('Run this check only on the remote Linux builder')
binary=Path(__file__).resolve().parent/'bin/winnowmap'
rng=random.Random(74031);reference=''.join(rng.choices('ACGT',k=80000))
with tempfile.TemporaryDirectory(prefix='winnowmap-index-roundtrip-') as temporary:
    root=Path(temporary);target=root/'reference.fasta';query=root/'queries.fasta';weights=root/'weights.txt'
    target.write_text('>ref\n'+reference+'\n')
    forward=reference[10000:30000];reverse=reference[45000:65000].translate(str.maketrans('ACGT','TGCA'))[::-1]
    query.write_text('>q0\n'+forward+'\n>q1\n'+reverse+'\n')
    weights.write_text(reference[12000:12015]+'\t1000\n')
    common=[str(binary),'-W',str(weights),'-k','15','-w','10','-t','4']
    subprocess.run(common+['-d',str(root/'target.mmi'),str(target)],check=True,capture_output=True,timeout=90)
    direct=subprocess.run(common+['-c','--secondary=no',str(target),str(query)],check=True,capture_output=True,timeout=90)
    loaded=subprocess.run(common+['-c','--secondary=no',str(root/'target.mmi'),str(query)],check=True,capture_output=True,timeout=90)
    rows=loaded.stdout.decode().splitlines()
    assert len(rows)==2 and {r.split('\t')[0] for r in rows}=={'q0','q1'}
    assert sorted(direct.stdout.splitlines())==sorted(loaded.stdout.splitlines()),('Native persisted-index output differs',direct.stdout.decode(),loaded.stdout.decode())
    print('Winnowmap native index round-trip: both strands, identical complete PAF/CIGAR/MAPQ output')
    partitioned=root/'partitioned.fasta'
    partition_reference(target,partitioned,40000,25000)
    subprocess.run(common+['-I','39999','-d',str(root/'parts.mmi'),str(partitioned)],check=True,capture_output=True,timeout=90)
    direct=subprocess.run(common+['-I','39999','-c','--secondary=no','--split-prefix',str(root/'direct_parts'),str(partitioned),str(query)],check=True,capture_output=True,timeout=90)
    loaded=subprocess.run(common+['-c','--secondary=no','--split-prefix',str(root/'loaded_parts'),str(root/'parts.mmi'),str(query)],check=True,capture_output=True,timeout=90)
    assert sorted(direct.stdout.splitlines())==sorted(loaded.stdout.splitlines()),('Multipart persisted-index output differs',direct.stdout.decode(),loaded.stdout.decode())
    output=root/'parts.paf';output.write_bytes(loaded.stdout);rows=list(parse(output))
    assert len(rows)==2 and {r['query'] for r in rows}=={0,1}, rows
    for row in rows:
        expected=(10001,30000) if row['query']==0 else (45001,65000)
        assert (row['tfirst'],row['tlast'])==expected,row
        assert row['cigar']=='20000M',row
    print('Winnowmap multipart round-trip: identical full PAF, both logical occurrences and CIGAR verified')
