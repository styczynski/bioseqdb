"""Native nucleotide MMseqs2 database/index/search workflow."""
CAPABILITIES={'window':False,'kmer':True,'mapq':False,'gapped_output':True}
COMMENTS=('MMseqs2 native nucleotide search (search-type 3), requested contiguous k, spaced seeds and masking disabled. '
          'No minimizer-window or MAPQ equivalent exists. Its necessary seed prefilter is part of this aligner, '
          'not the disabled optional Bioseq prefilter. Gap open/extension 4/2; configurable nucleotide matrix 2/-4 matches baseline scoring. '
          'Native nucleotide default splits at 10000 bp, shorter than these queries; max-seq-len 65535 with '
          '32768 bp target overlap preserves complete approximately 20 kb occurrences. Native offsetalignment restores coordinates; '
          'native filterdb selects the first score-ranked hit per query, matching primary-only baseline reporting. '
          'Query coverage 0.8 suppresses short chance HSPs, as qualified for BLAST. '
          'Alignment backtrace is requested and audited. B subtracts Matcher getSWResult intervals; stage scope differs from minimap2.')


def seed_options(root,config):
    return ['--search-type','3','-k',str(config['data']['kmer_size']),
            '--spaced-kmer-mode','0','--mask','0','--threads',str(config['threads']),
            '--max-seq-len','65535','--sequence-overlap','32768',
            '--seed-sub-mat',str(root/'bench/tool_mmseqs2/nucleotide_baseline.out')]


def setup(root,config,fixture,index,run):
    binary=str(root/'bench/tool_mmseqs2/bin/mmseqs')
    run([binary,'createdb',str(fixture/'target.fasta'),str(index/'target'),'--dbtype','2','--shuffle','0'])
    run([binary,'createindex',str(index/'target'),str(index/'tmp'),*seed_options(root,config)])


def align(root,config,fixture,index,query,work,run):
    binary=str(root/'bench/tool_mmseqs2/bin/mmseqs')
    output=work/'alignments.tsv'
    run([binary,'createdb',str(query),str(work/'query'),'--dbtype','2','--shuffle','0'])
    run([binary,'search',str(work/'query'),str(index/'target'),str(work/'result'),str(work/'tmp'),
         *seed_options(root,config),'--sub-mat',str(root/'bench/tool_mmseqs2/nucleotide_baseline.out'),
         '-a','1','--gap-open','4','--gap-extend','2','-c','0.8','--cov-mode','2'])
    run([binary,'filterdb',str(work/'result'),str(work/'primary'),'--extract-lines','1'])
    run([binary,'convertalis',str(work/'query'),str(index/'target'),str(work/'primary'),str(output),
         '--format-output','query,target,qstart,qend,tstart,tend,qaln,taln','--threads',str(config['threads'])])
    return output


def parse(output):
    for line in output.read_text().splitlines():
        fields=line.split('\t')
        if len(fields)!=8: raise ValueError('Malformed MMseqs2 alignment')
        query,target,qs,qe,ts,te,qaln,taln=fields
        yield {'query':int(query.removeprefix('q')),'target':target,
            'qfirst':int(qs),'qlast':int(qe),'tfirst':int(ts),'tlast':int(te),
            'qaln':qaln,'taln':taln,'mapq':None}
