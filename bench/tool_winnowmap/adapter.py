"""Winnowmap weighted-minimizer index and separate mapping process."""
import json
from pathlib import Path
import re

CAPABILITIES={'window':True,'kmer':True,'mapq':True,'gapped_output':True}
COMMENTS=('Winnowmap weighted minimizers with native minimal confidently alignable substrings. '
          'Requested k/w retained; scoring 2/-4, dual gaps 4,24/2,1 and first-stage bandwidth 500 match the baseline. '
          'Native SV-aware second stage retains its bandwidth 2000 and maximum gap 16000; these additional stages have no baseline counterpart. '
          'Seed occurrence fraction 0.0002 and minimum occurrence floor 10 match the baseline. The default index batch limit is 8 Gbp, with the memory bounds below. '
          'Meryl count and high-frequency k-mer selection are index setup, included in no_index. '
          'For 4 Gbp k20 inputs on hosts below 10 GiB RAM, Meryl uses native memory=4 batching after a demonstrated OOM; reference layout and k-mer counts are unchanged. '
          'The native -d output path is re-enabled; loaded indices reconstruct the identical native weighting Bloom filter from the saved k-mer list. '
          'A remote fresh-reference versus persisted-index output parity test is required after building. '
          'The native MCAS outer repeat length is initialized to zero to remove undefined MAPQ/output behavior. '
          'For dna4 k15 and 2 Gbp dna5 k15 on hosts below 10 GiB RAM, demonstrated single-record indexing OOMs require last-resort 250 Mb FASTA records '
          'overlapping by longest query plus 64 bases, native -I 499M index batches and --split-prefix global result merging. '
          'Meryl weights still come from the original reference. Partition preparation is included in no_index; '
          'native per-part frequency estimation and overlapping records can affect MAPQ. Other profiles retain the original FASTA layout; '
          'low-memory hosts and all 4 Gbp inputs use native -I 1999M batching, merging the two native parts for 4 Gbp. '
          'No secondary alignments are printed, matching baseline reporting. Native MAPQ is audited for presence, not numeric equality.')


def partition_reference(source_path,target,maximum,overlap):
    if not 0<overlap<maximum:raise ValueError('Invalid reference partition overlap')
    buffered=bytearray();offset=records=0
    with source_path.open('rb') as source,target.open('wb') as output:
        for line in source:
            if line.startswith(b'>'):continue
            buffered.extend(line.strip())
            while len(buffered)>=maximum:
                output.write(('>ref_offset'+str(offset)+'\n').encode())
                for start in range(0,maximum,1048576):output.write(buffered[start:min(start+1048576,maximum)]+b'\n')
                advance=maximum-overlap;del buffered[:advance];offset+=advance;records+=1
        if buffered:
            output.write(('>ref_offset'+str(offset)+'\n').encode())
            for start in range(0,len(buffered),1048576):output.write(buffered[start:start+1048576]+b'\n')
            records+=1
    return records


def setup(root,config,fixture,index,run):
    binary=str(root/'bench/tool_winnowmap/bin/winnowmap')
    meryl=str(root/'bench/tool_winnowmap/bin/meryl')
    k=str(config['data']['kmer_size']); w=str(config['data']['window_size'])
    memory_kib=int(next(line.split()[1] for line in Path('/proc/meminfo').read_text().splitlines() if line.startswith('MemTotal:')))
    low_memory=memory_kib<10*1024*1024
    meryl_memory=['memory=4'] if low_memory and config['data']['reference_length']==4_000_000_000 and config['data']['kmer_size']==20 else []
    run([meryl,'count','k='+k,'threads='+str(config['threads']),*meryl_memory,'output',str(index/'meryl'),str(fixture/'target.fasta')])
    run([meryl,'print','greater-than','distinct=0.9998',str(index/'meryl')],stdout=index/'repetitive.txt')
    two_input_records=config['data']['reference_length']>2000000352
    demonstrated_oom=config['data']['kmer_size']==15 and (config['data']['alphabet']=='dna4' or
        (config['data']['alphabet']=='dna5' and config['data']['reference_length']==2_000_000_000))
    partitioned=low_memory and demonstrated_oom
    layout={'partitioned':partitioned,'index_batch':'1999M' if low_memory or two_input_records else '8G','memory_kib':memory_kib,
            'multipart':partitioned or two_input_records}
    target=fixture/'target.fasta'
    if layout['partitioned']:
        maximum=250000000
        overlap=max(len(line.strip()) for line in (fixture/'queries.fasta').read_text().splitlines() if not line.startswith('>'))+64
        target=index/'reference.fasta'
        records=partition_reference(fixture/'target.fasta',target,maximum,overlap)
        layout.update({'index_batch':'499M','maximum_record_bases':maximum,'overlap_bases':overlap,'records':records,
                       'reason':'Single 2 Gbp record exhausted RAM and zram; native -I does not split an individual record'})
    run([binary,'-W',str(index/'repetitive.txt'),'-k',k,'-w',w,'-I',layout['index_batch'],'-t',str(config['threads']),
         '-d',str(index/'target.mmi'),str(target)])
    (index/'layout.json').write_text(json.dumps(layout,indent=2))
    return layout


def align(root,config,fixture,index,query,work,run):
    output=work/'alignments.paf'
    layout=json.loads((index/'layout.json').read_text())
    split=['--split-prefix',str(work/'native_split')] if layout.get('multipart',layout['partitioned']) else []
    run([str(root/'bench/tool_winnowmap/bin/winnowmap'),'-W',str(index/'repetitive.txt'),
         '-k',str(config['data']['kmer_size']),'-w',str(config['data']['window_size']),
         '-t',str(config['threads']),'-A','2','-B','4','-O','4,24','-E','2,1','-r','500',
         '-f','0.0002','--min-occ-floor','10',
         '-c','--secondary=no',*split,str(index/'target.mmi'),str(query)],stdout=output)
    return output


def parse(output):
    for line in output.read_text().splitlines():
        fields=line.split('\t')
        if len(fields)<12: raise ValueError('Malformed Winnowmap PAF')
        query,qlen,qs,qe,strand,target,tlen,ts,te,_,_,mapq=fields[:12]
        tags={t[:2]:t[5:] for t in fields[12:]}
        if tags.get('tp')=='S': raise ValueError('Unexpected secondary result')
        offset=0
        if re.fullmatch(r'ref_offset[0-9]+',target):
            offset=int(target.removeprefix('ref_offset'));target='ref_part0'
        yield {'query':int(query.removeprefix('q')),'target':target,
               'qfirst':int(qs)+1 if strand=='+' else int(qe),
               'qlast':int(qe) if strand=='+' else int(qs)+1,
               'tfirst':int(ts)+1+offset,'tlast':int(te)+offset,'cigar':tags.get('cg'), 'mapq':int(mapq),
               'edit_distance':int(tags['NM']) if 'NM' in tags else None}
