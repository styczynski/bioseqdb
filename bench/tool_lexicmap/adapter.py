"""LexicMap index/search with explicit native parameter qualifications."""
import csv
import re

CAPABILITIES={'window':False,'kmer':True,'mapq':False,'gapped_output':True}
COMMENTS=('LexicMap uses LexicHash masks, not minimizer windows; requested k is retained and w is inapplicable. '
          'Native mask count 20000 and desert filling retained. Intrinsic low-complexity k-mer rejection is retained. '
          'Seed prefix lengths are bounded by requested k. '
          'The native 28-bit position limit rejects contigs over 268435456 bases. '
          'Input is therefore split into 268435455-base contigs overlapping by the longest actual query, '
          'without consulting occurrence truth; returned coordinates are restored to the logical reference. '
          'FASTA partitioning is index preparation and is measured in no_index. Native query coverage threshold is 80 percent. '
          'Native WFA costs are match 0, mismatch 4, gap open 6 and extension 2. '
          'Its separate reported-score/E-value calculation uses match 2, mismatch -3 and gaps 5/2. '
          'These settings are fixed in the implementation and are not rewritten for benchmarking. '
          'WFA uses literal base equality, including N/N, rather than a separate ambiguous-base penalty. '
          'The native two-bit reference store converts N to A before alignment; queries retain their input bases. '
          'Gapped replay checks this exact reference encoding before reverse complementation and reports affected base counts. '
          'No MAPQ output exists. All gapped output is requested. B subtracts instrumented WFA alignment calls; '
          'this is a tool-specific boundary, not equivalent to minimap2 fused alignment.')


def partition_reference(source_path,target,maximum,overlap):
    if not 0<overlap<maximum:raise ValueError('Invalid reference partition overlap')
    buffered=bytearray(); offset=0
    with source_path.open('rb') as source,target.open('wb') as output:
        for line in source:
            if line.startswith(b'>'):continue
            buffered.extend(line.strip())
            while len(buffered)>=maximum:
                output.write(('>ref_offset'+str(offset)+'\n').encode())
                for start in range(0,maximum,1048576):output.write(buffered[start:min(start+1048576,maximum)]+b'\n')
                advance=maximum-overlap;del buffered[:advance];offset+=advance
        if buffered:
            output.write(('>ref_offset'+str(offset)+'\n').encode())
            for start in range(0,len(buffered),1048576):output.write(buffered[start:start+1048576]+b'\n')


def setup(root,config,fixture,index,run):
    maximum=268435455
    overlap=max(len(line.strip()) for line in (fixture/'queries.fasta').read_text().splitlines() if not line.startswith('>'))
    target=index/'target.fasta'
    partition_reference(fixture/'target.fasta',target,maximum,overlap)
    run([str(root/'bench/tool_lexicmap/bin/lexicmap'),'index',str(target),
         '-O',str(index/'target'),'-g',str(maximum),
         '-k',str(config['data']['kmer_size']),'-j',str(config['threads'])])


def align(root,config,fixture,index,query,work,run):
    output=work/'alignments.tsv'; k=config['data']['kmer_size']
    run([str(root/'bench/tool_lexicmap/bin/lexicmap'),'search','-d',str(index/'target'),
         '-j',str(config['threads']),'-p',str(min(15,k)),'-P',str(min(17,k)),
         '-a','-q','80','-o',str(output),str(query)])
    return output


def parse(output):
    with output.open() as stream:
        for row in csv.DictReader(stream,delimiter='\t'):
            if not re.fullmatch(r'ref_offset[0-9]+',row['sseqid']):raise ValueError('Unknown LexicMap reference identifier')
            offset=int(row['sseqid'].removeprefix('ref_offset'))
            first,last=int(row['sstart']),int(row['send'])
            if row['sstr']=='-' and first<last: first,last=last,first
            yield {'query':int(row['query'].removeprefix('q')),'target':'ref_part0',
                   'qfirst':int(row['qstart']),'qlast':int(row['qend']),
                   'tfirst':first+offset,'tlast':last+offset,'qaln':row['qseq'],'taln':row['sseq'],
                   'cigar':row['cigar'],'mapq':None,'reference_encoding':'lexicmap_two_bit'}
