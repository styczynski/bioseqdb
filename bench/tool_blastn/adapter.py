"""NCBI blastn command and gapped-result adapter."""
from pathlib import Path

CAPABILITIES = {'window': False, 'kmer': True, 'mapq': False, 'gapped_output': True}
COMMENTS = ('NCBI blastn 2.17.0+: word_size equals requested k. No minimizer-window or MAPQ equivalent exists. '
    'Reward 2, penalty -4, gap open 4 and extension 2 match the baseline first affine branch; BLAST has no dual-affine branch. '
    'Native ambiguity averaging gives N comparisons score -3 with these reward/penalty settings, versus baseline -1; '
    'the blastn CLI does not expose an independent ambiguity score. '
    'DUST and soft masking are disabled as in the baseline. E-value retains native default 10. '
    'qcov_hsp_perc=80 suppresses short chance HSPs that BLAST otherwise reports alongside full-length hits; '
    'the initial q2 audit exposed 35-base off-target HSPs. This output qualification preserves k and scoring. '
    'Gapped query/subject strings provide an auditable edit script. B subtracts physical traceback-search intervals; '
    'that stage differs from the minimap2 fused DP/CIGAR boundary. No synthetic MAPQ is reported.')


def setup(root, config, fixture, index, run):
    run([str(root/'bench/tool_blastn/bin/makeblastdb'), '-in',str(fixture/'target.fasta'),
         '-dbtype','nucl','-parse_seqids','-out',str(index/'target')])


def align(root, config, fixture, index, query, work, run):
    output=work/'alignments.tsv'
    run([str(root/'bench/tool_blastn/bin/blastn'), '-task','blastn','-query',str(query),
        '-db',str(index/'target'),'-word_size',str(config['data']['kmer_size']),
        '-reward','2','-penalty','-4','-gapopen','4','-gapextend','2',
        '-dust','no','-soft_masking','false','-num_threads',str(config['threads']),
        '-qcov_hsp_perc','80',
        '-outfmt','6 qseqid sseqid qstart qend sstart send qseq sseq score evalue',
        '-out',str(output)])
    return output


def parse(output):
    for line in output.read_text().splitlines():
        fields=line.split('\t')
        if len(fields)!=10: raise ValueError('Malformed blastn output')
        query,target,qs,qe,ts,te,qaln,taln,score,evalue=fields
        yield {'query':int(query.removeprefix('q')),'target':target.split('|')[-1],
            'qfirst':int(qs),'qlast':int(qe),'tfirst':int(ts),'tlast':int(te),
            'qaln':qaln,'taln':taln,'mapq':None}
