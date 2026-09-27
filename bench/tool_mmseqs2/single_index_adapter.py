"""Native MMseqs2 nucleotide linear index/search, with fixed requested k."""
import hashlib
import json
import os
CAPABILITIES={'window':False,'kmer':True,'mapq':False,'gapped_output':True}
COMMENTS=('MMseqs2 native createlinindex, query-centric kmersearch, align and offsetalignment nucleotide workflow (search-type 3). Requested contiguous k is retained; '
          'automatic k adjustment, spaced seeds, masking and composition bias correction are disabled. '
          'No minimizer-window or MAPQ equivalent exists. Its necessary seed prefilter is part of this aligner, '
          'not the disabled optional Bioseq prefilter. Gap open/extension 4/2; configurable nucleotide matrix 2/-4 matches baseline scoring. '
          'Configurable Z-drop is 400, matching the baseline first extension threshold. Native alignment band is hard-coded to 64 and retained. '
          'Native linear-index positions use signed 16-bit storage; target chunks are 32734 bp and overlap by the longest actual query. '
          'This preserves complete approximately 20 kb occurrences. Native offsetalignment restores coordinates; '
          'native filterdb selects the first score-ranked hit per query, matching primary-only baseline reporting. '
          'Query coverage 0.8 suppresses short chance HSPs, as qualified for BLAST. '
          'Native kmersearch result-direction 0 keeps the original query/target orientation and cov-mode 2 applies to the query. '
          'The linsearch wrapper reverse-strand pilot produced shifted coordinates and was rejected; native query-centric components avoid that wrapper. '
          'Alignment backtrace is requested and audited. B subtracts Matcher getSWResult intervals; stage scope differs from minimap2. '
          'The standard dense index was not selected: its k20 offset table alone needs 8 TiB, exceeding every requested host. '
          'Native linear sampling defaults are retained; minimizer window size has no corresponding parameter.')
COMMENTS+=' For dna5, the configurable ambiguous-base X/N row and column use -1, matching baseline sc_ambi=1.'


def matrix_path(root,config):
    filename='nucleotide_baseline_dna5.out' if config['data']['alphabet']=='dna5' else 'nucleotide_baseline.out'
    return root/'bench/tool_mmseqs2'/filename


def seed_options(root,config):
    return ['--search-type','3','-k',str(config['data']['kmer_size']),
            '--spaced-kmer-mode','0','--mask','0','--threads',str(config['threads']),
            '--max-seq-len','32734',
            '--seed-sub-mat',str(matrix_path(root,config))]


def setup(root,config,fixture,index,run):
    binary=str(root/'bench/tool_mmseqs2/bin/mmseqs')
    longest=max(len(line.strip()) for line in (fixture/'queries.fasta').read_text().splitlines() if not line.startswith('>'))
    if longest>=32734:raise ValueError('Query exceeds native signed-position chunk capacity')
    options=[*seed_options(root,config),'--sequence-overlap',str(longest),'--adjust-kmer-len','0']
    # A failed query-parameter pilot may already have completed exactly this index.
    # Reuse is restricted to just_index and still invokes native compatibility checks.
    if config['mode']=='just_index':
        binary_hash=hashlib.sha256((root/'bench/tool_mmseqs2/bin/mmseqs').read_bytes()).hexdigest()
        identity=json.loads((fixture/'identity.json').read_text())
        for report_path in sorted((root/'bench/e2e_external/results').glob('mmseqs2*/*/report.json'),reverse=True):
            prior=json.loads(report_path.read_text());commands=prior.get('untimed_index_setup',[])
            if prior['status']!='invalid' or not commands or not all(c['exit_code']==0 for c in commands):continue
            if prior.get('fixture',{}).get('target_sha256')!=identity['target_sha256']:continue
            if prior['artifacts'].get('bench/tool_mmseqs2/bin/mmseqs')!=binary_hash:continue
            if prior['artifacts'].get(str(matrix_path(root,config).relative_to(root)))!=hashlib.sha256(matrix_path(root,config).read_bytes()).hexdigest():continue
            if commands[-1]['argv'][1]!='createlinindex' or commands[-1]['argv'][4:]!=options:continue
            previous=report_path.parent/'index'
            if not (previous/'target.linidx').is_file():continue
            for path in previous.iterdir():
                if path.is_file() and path.name.startswith('target') and not path.name.startswith('target_split_'):
                    os.link(path.resolve(),index/path.name)
            check_options=list(options)
            # Native metadata stores the matrix basename; its compatibility check
            # compares that name literally. Matrix bytes were checked above.
            check_options[check_options.index('--seed-sub-mat')+1]=matrix_path(root,config).name
            run([binary,'createlinindex',str(index/'target'),str(index/'tmp'),*check_options,'--check-compatible','2'])
            return {'reused_completed_index_from_failed_query_pilot':str(report_path),
                    'report_sha256':hashlib.sha256(report_path.read_bytes()).hexdigest(),
                    'native_index_compatibility_checked':True}
    run([binary,'createdb',str(fixture/'target.fasta'),str(index/'target'),'--dbtype','2','--shuffle','0'])
    run([binary,'createlinindex',str(index/'target'),str(index/'tmp'),*options])


def align(root,config,fixture,index,query,work,run):
    binary=str(root/'bench/tool_mmseqs2/bin/mmseqs')
    output=work/'alignments.tsv'
    run([binary,'createdb',str(query),str(work/'query'),'--dbtype','2','--shuffle','0'])
    run([binary,'kmersearch',str(work/'query'),str(index/'target.linidx'),str(work/'prefilter'),
         '--result-direction','0','--mask','0','--threads',str(config['threads']),
         '--max-seq-len','32734','--seed-sub-mat',str(matrix_path(root,config))])
    run([binary,'align',str(work/'query'),str(index/'target.linidx'),str(work/'prefilter'),str(work/'aligned'),
         '--threads',str(config['threads']),'--max-seq-len','32734','--comp-bias-corr','0',
         '--sub-mat',str(matrix_path(root,config)),
         '-a','1','--gap-open','4','--gap-extend','2','--zdrop','400','-c','0.8','--cov-mode','2'])
    run([binary,'offsetalignment',str(work/'query'),str(work/'query'),
         str(index/'target.linidx'),str(index/'target.linidx'),str(work/'aligned'),str(work/'result'),
         '--search-type','3','--threads',str(config['threads'])])
    run([binary,'filterdb',str(work/'result'),str(work/'primary'),'--extract-lines','1'])
    run([binary,'convertalis',str(work/'query'),str(index/'target'),str(work/'primary'),str(output),
         '--search-type','3','--sub-mat',str(matrix_path(root,config)),
         '--gap-open','4','--gap-extend','2',
         '--format-output','query,target,qstart,qend,tstart,tend,qaln,taln,bits','--threads',str(config['threads'])])
    return output


def parse(output):
    for line in output.read_text().splitlines():
        fields=line.split('\t')
        if len(fields)!=8: raise ValueError('Malformed MMseqs2 alignment')
        query,target,qs,qe,ts,te,qaln,taln=fields
        yield {'query':int(query.removeprefix('q')),'target':target,
            'qfirst':int(qs),'qlast':int(qe),'tfirst':int(ts),'tlast':int(te),
            'qaln':qaln,'taln':taln,'mapq':None}
