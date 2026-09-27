#!/usr/bin/env python3
"""Recheck raw external observations and emit the host's self-contained tuple file."""
import json
from pathlib import Path
import sys
import shutil
import tarfile
import importlib.util
import hashlib
import yaml

from campaign import AREA, ROOT, TOOLS, digest, measure_intervals, read_fasta, validate

CAMPAIGN='bioseq-e2e-external-algorithm-matrix-v1'
FIELDS=['campaign_id','dataset','alphabet','target_size_gbp','kmer_size','window_size',
        'index_mode','engine','bioseq_index_name','requested_minimizer_algorithm',
        'requested_chaining_algorithm','requested_base_alignment_algorithm']


def archive_evidence(report_path):
    destination=report_path.parent/'evidence.tar.gz'
    if not destination.exists():
        temporary=destination.with_suffix('.tmp')
        files=[report_path,*report_path.parent.glob('q*.fasta')]
        for directory in report_path.parent.iterdir():
            if directory.is_dir() and (directory.name=='setup' or directory.name.startswith('q')):
                files.extend(p for p in directory.iterdir() if p.is_file() and
                    (p.name in ['commands.json','intervals.tsv','alignments.tsv','alignments.paf'] or p.suffix in ['.stdout','.stderr']))
                files.extend(directory.glob('group*/alignments.tsv'))
        with tarfile.open(temporary,'w:gz',compresslevel=1) as archive:
            for path in sorted(files):archive.add(path,arcname=str(path.relative_to(report_path.parent)),recursive=False)
        temporary.replace(destination)
    return {'path':str(destination.relative_to(ROOT)),'sha256':digest(destination)}


def archive_fixture(fixture):
    destination=fixture/'evidence.tar.gz'
    if not destination.exists():
        temporary=destination.with_suffix('.tmp')
        with tarfile.open(temporary,'w:gz',compresslevel=1) as archive:
            for name in ['identity.json','truth.tsv','queries.fasta']:
                archive.add(fixture/name,arcname=name,recursive=False)
        temporary.replace(destination)
    return {'path':str(destination.relative_to(ROOT)),'sha256':digest(destination),
            'contents':['identity.json','truth.tsv','queries.fasta'],
            'reference_scope':'Truth includes the original reference window for each expected occurrence; full reference FASTA is reproducible from the pinned generator and seed.'}


def key(config):
    data=config['data']
    values=[CAMPAIGN,config['dataset'],data['alphabet'],data['reference_length']//1000000000,
            data['kmer_size'],data['window_size'],config['mode'],config['tool'],None,None,None,None]
    return 'bioseq-e2e-cell-v2:'+json.dumps(values,separators=(',',':'))


def audit_signature():
    sources=[Path(__file__),AREA/'campaign.py']
    for tool in TOOLS:sources.extend((ROOT/('bench/tool_'+tool)).glob('*.py'))
    return {str(p.relative_to(ROOT)):digest(p) for p in sorted(set(sources))}


def review_seal(value):
    return hashlib.sha256(json.dumps(value,sort_keys=True,separators=(',',':')).encode()).hexdigest()


def load_review_cache(path,signature,root=ROOT):
    try:
        cached=json.loads(path.read_text());seal=cached.pop('seal_sha256')
        if review_seal(cached)!=seal or cached['signature']!=signature:return None
        for filename,expected in cached['files'].items():
            evidence=(root/filename).resolve()
            if not evidence.is_relative_to(root.resolve()) or digest(evidence)!=expected:return None
        return cached['row']
    except (OSError,ValueError,KeyError):return None


def save_review_cache(path,signature,row,files):
    cached={'signature':signature,'row':row,'files':files}
    cached['seal_sha256']=review_seal(cached)
    path.parent.mkdir(parents=True,exist_ok=True)
    temporary=path.with_suffix('.tmp');temporary.write_text(json.dumps(cached));temporary.replace(path)


def review(path,fixture_digests=None):
    if fixture_digests is None:fixture_digests={}
    report=json.loads(path.read_text());config=report['config']
    fixture=AREA/'fixtures'/config['dataset']
    for filename,key in [('target.fasta','target_sha256'),('queries.fasta','queries_sha256'),('truth.tsv','truth_sha256')]:
        candidate=fixture/filename
        if candidate not in fixture_digests:fixture_digests[candidate]=digest(candidate)
        assert fixture_digests[candidate]==report['fixture'][key], 'Fixture digest mismatch'
    signature={'audit_sources':audit_signature(),'report_sha256':digest(path)}
    cache=AREA/'.state/review_cache'/(hashlib.sha256(str(path.relative_to(ROOT)).encode()).hexdigest()+'.json')
    cached=load_review_cache(cache,signature)
    if cached is not None:return cached
    row=review_full(path,fixture_digests)
    files={}
    for rung in report['rungs']:
        for rep in rung['repeats']:
            directory=path.parent/f'q{rung["query_count"]}_repeat{rep["repeat"]}'
            for name in ['intervals.tsv','alignments.paf' if config['tool']=='winnowmap' else 'alignments.tsv']:
                evidence=directory/name;files[str(evidence.relative_to(ROOT))]=digest(evidence)
    save_review_cache(cache,signature,row,files)
    return row


def review_full(path,fixture_digests=None):
    if fixture_digests is None:fixture_digests={}
    report=json.loads(path.read_text()); config=report['config']
    if report['status']!='validated_pending_review': raise ValueError('Incomplete or invalid report')
    assert config['tool'] in TOOLS and config['benchmark']=='e2e_external'
    assert [r['query_count'] for r in report['rungs']]==[2,32,1024]
    assert [len(r['repeats']) for r in report['rungs']]==[1,1,5]
    fixture=AREA/'fixtures'/config['dataset']
    truth=[line.split('\t') for line in (fixture/'truth.tsv').read_text().splitlines()]
    spec=importlib.util.spec_from_file_location(config['tool']+'_review',ROOT/('bench/tool_'+config['tool']+'/adapter.py'))
    adapter=importlib.util.module_from_spec(spec);spec.loader.exec_module(adapter)
    query_sequences=read_fasta(fixture/'queries.fasta')
    replay_truth=[{'start':int(f[1]),'end':int(f[2]),'reverse':bool(int(f[3])),
                   'window_start':int(f[5]),'window':f[6]} for f in truth]
    for filename,k in [('target.fasta','target_sha256'),('queries.fasta','queries_sha256'),('truth.tsv','truth_sha256')]:
        candidate=fixture/filename
        if candidate not in fixture_digests:fixture_digests[candidate]=digest(candidate)
        assert fixture_digests[candidate]==report['fixture'][k], 'Fixture digest mismatch'
    pids=[]
    for rung in report['rungs']:
        assert rung['passed']
        for rep in rung['repeats']:
            audit=rep['audit']; count=rung['query_count']
            assert rep['valid'] and audit['valid'] and audit['match_counts']==[1]*count
            assert audit['gapped_replay_count']==count and audit['max_position_error_bp']<15
            assert len(audit['occurrences'])==count
            for ordinal,hit in enumerate(audit['occurrences']):
                expected=truth[ordinal]
                assert hit['query']==ordinal
                assert abs(hit['start']-int(expected[1]))<15 and abs(hit['end']-int(expected[2]))<15
                assert hit['reverse']==bool(int(expected[3]))
            directory=path.parent/f'q{count}_repeat{rep["repeat"]}'
            output=directory/('alignments.paf' if config['tool']=='winnowmap' else 'alignments.tsv')
            assert digest(output)==rep['output_sha256'], 'Output changed after audit'
            replay=validate(adapter.parse(output),replay_truth,query_sequences,count,adapter.CAPABILITIES['mapq'])
            if config['tool']=='winnowmap':
                assert replay['native_edit_distance_checked_count']==count
            assert replay['occurrences']==audit['occurrences']
            commands=rep['commands']; assert commands and all(c['exit_code']==0 for c in commands)
            if config['mode']=='just_index':
                assert report['untimed_index_setup']
                assert all('makeblastdb'!=Path(c['argv'][0]).name for c in commands)
            begin=min(c['begin_ns'] for c in commands); end=max(c['end_ns'] for c in commands)
            physical=measure_intervals(directory/'intervals.tsv',begin,end,config['tool'])
            assert physical['cigar_or_mapq_union_ns']==rep['cigar_or_mapq_union_ns']
            assert 0<rep['b_ns']<=rep['a_ns'] and rep['b_ns']==rep['a_ns']-physical['cigar_or_mapq_union_ns']
            pids.append(commands[-1]['pid'])
    assert len(set(pids))==len(pids), 'Aligner process was reused'
    final=report['rungs'][-1]['repeats']
    metrics={'unit':'ns','p50_a':sorted(r['a_ns'] for r in final)[2],
             'p50_b':sorted(r['b_ns'] for r in final)[2]}
    assert report['metrics']==metrics and report['run_count']==5
    data=config['data']; null_algorithms={'minimizer':None,'chaining':None,'base_alignment':None}
    comments=report['comments']
    if config['tool']=='lexicmap':
        comments=comments.replace('Native WFA scoring is 2/-3 with gaps 5/2, fixed in the implementation and not rewritten for benchmarking.',
            'Native WFA costs are match 0, mismatch 4, gap open 6 and extension 2. The separate reported-score/E-value calculation uses match 2, mismatch -3 and gaps 5/2. These native settings are not rewritten for benchmarking; WFA uses literal base equality, including N/N.')
    host=report.get('host_binding') or json.loads((AREA/'initial_host_binding.json').read_text())
    return {'stable_key_schema':'bioseq-e2e-cell-v2','stable_key':key(config),
        'timing_scope':'external_process_iteration_v1','campaign_id':CAMPAIGN,
        'dataset':config['dataset'],'target_size_gbp':data['reference_length']//1000000000,
        'alphabet':data['alphabet'],'kmer_size':data['kmer_size'],'window_size':data['window_size'],
        'query_count':1024,'mode':config['mode'],'pipeline':config['tool'],
        'measurement_host_binding':host,
        'pipeline_mode':None,'index_type':config['tool']+'_native',
        'requested_algorithms':null_algorithms,'observed_algorithms':null_algorithms,
        'effective_path':config['tool'],'execution_conformance':'native_external_aligner',
        'comparison_eligibility':{'requested_path':True,'effective_path':True,
            'scope':'audited_occurrences_with_native_algorithm_and_metric_B_qualifications'},
        'prefilter':None if config['tool'] in ['mmseqs2','lexicmap'] else False,
        'exact_baseline_match_count_equal':True,'status':'accepted','run_count':5,'metrics':metrics,
        'capabilities':report['capabilities'],'comments':comments,
        'alignment_scoring_qualification':{
            'native_wfa_costs':{'match':0,'mismatch':4,'gap_open':6,'gap_extend':2},
            'reported_score_parameters':{'match':2,'mismatch':-3,'gap_open':5,'gap_extend':2},
            'ambiguity_handling':'reference N is stored as A before reverse complementation; query bases are retained; WFA compares literal bytes',
            'independent_cli_controls_available':False}
            if config['tool']=='lexicmap' else None,
        'ambiguity_scoring_qualification':{'baseline_n_score':-1,'native_n_score':-3,
            'reason':'blastn derives ambiguity scores from reward/penalty; no independent score was exposed by its CLI'}
            if config['tool']=='blastn' and data['alphabet']=='dna5' else None,
        'evidence':{'report_sha256':digest(path),'artifacts':report['artifacts'],
          'publication_cigar_nm_replay':True if config['tool']=='winnowmap' else None,
          'publication_gapped_replay':True,
          'tool_source':report['tool_source'],'fixture':report['fixture'],
          'memory_environment':{k:report['host'].get(k,'not_recorded_before_zram_audit') for k in ['memory','swap','zram']},
          'reference_layout':report.get('reference_layout',{'input_records':1 if data['reference_length']<=2000000352 else 2}),
          'setup_provenance':report.get('setup_provenance'),
          'untimed_index_setup':report.get('untimed_index_setup'),
          'build_receipt':report.get('build_receipt'),
          'repeats':[{'a_ns':r['a_ns'],'b_ns':r['b_ns'],
            'cigar_or_mapq_union_ns':r['cigar_or_mapq_union_ns'],'regions_ns':r['regions_ns'],
            'interval_counts':r['interval_counts'],'output_sha256':r['output_sha256'],
            'max_position_error_bp':r['audit']['max_position_error_bp'],
            'gapped_replay_count':r['audit']['gapped_replay_count'],
            'native_encoded_reference_bases_checked':r['audit'].get('native_encoded_reference_bases_checked',0),
            'memory_before':r.get('memory_before'),'memory_after':r.get('memory_after'),
            'index_preparation_metadata':r.get('index_preparation_metadata'),
            'match_counts':r['audit']['match_counts'],'commands':r['commands']} for r in final]}}


def main():
    binding=json.loads((AREA/'host_binding.json').read_text())
    rows={}; qualifications=[]; invalid=[]; host_inventory=None; fixture_digests={}
    for path in sorted((AREA/'results').glob('*/*/report.json')):
        report=json.loads(path.read_text())
        host_inventory=report.get('host',host_inventory)
        if report['status']=='validated_pending_review':
            row=review(path,fixture_digests)
            row['evidence']['archive']=archive_evidence(path)
            row['evidence']['fixture_archive']=archive_fixture(AREA/'fixtures'/report['config']['dataset'])
            storage=row['measurement_host_binding']['storage']
            if storage['Iops']==6000 and storage['Throughput']==500:
                rows[row['stable_key']]=row
            else:
                row['status']='qualification_only'
                row['timing_scope']='external_process_iteration_initial_storage_v1'
                row['comments']+=' Initial provisioning storage had 3000 IOPS/125 MiB/s; retained as qualification evidence and excluded from the matched-storage matrix.'
                qualifications.append(row)
        elif report['status']=='invalid':
            invalid.append({'stable_key':key(report['config']),'status':'invalid',
                'reason':report['error'],'report_sha256':digest(path),'archive':archive_evidence(path)})
    configs=[json.loads(p.read_text()) for p in (AREA/'configs').glob('*.json')]
    eligible=sorted(key(c) for c in configs)
    assert len(eligible)==64 and len(set(eligible))==64, 'External matrix must contain exactly 64 unique cells'
    assert set(rows).issubset(eligible), 'Accepted observation is outside the configured external matrix'
    result={'schema':'bioseq.remote-approved-e2e-result-tuples','version':1,
      'status':'approved_remote_complete' if len(rows)==len(eligible) else 'approved_remote_partial',
      'purpose':'External-only supplementary measurements. Whole fresh-process wall time A includes executable startup, persisted index loading, alignment and output. no_index additionally constructs a new on-disk index inside A; just_index constructs it outside A in separate processes. Fixture generation, audit and digest checks are outside A. Five q1024 repetitions follow audited q2 and q32 gates. B subtracts the same-repeat physical union of native alignment and available MAPQ regions; native boundaries and unsupported parameters are qualified per row. No MAPQ is invented. OS page caches are unspecified and not purged. All timings are integer nanoseconds. Published baseline match-count contract is one primary occurrence per query; external outputs independently pass that vector, position, strand and edit-script audits. No minimap2 or Bioseq benchmark was run by this campaign.',
      'authority':{'contains_only_approved_rows':True,'full_eligible_set_complete':len(rows)==len(eligible),
                   'host_timings_only':True,'external_lookup_required_to_interpret':False,
                   'profile':'aws_external_aligners_v1','normative_datasets':['approved_results'],
                   'qualification_results_are_non_normative':True},
      'remote_host_binding':binding,'observed_host_inventory':host_inventory,
      'campaign_identity':{'campaign_id':CAMPAIGN},
      'stable_key_contract':{'ordered_aggregator_fields':FIELDS,'stable_key_schema':'bioseq-e2e-cell-v2',
            'observation_identity_rule':'Unique ordered pair (stable_key, timing_scope).'},
      'matrix_scope':{'pipelines':['blastn','mmseqs2','winnowmap','lexicmap'],
          'query_count':1024,'query_ladder':[2,32,1024],'repetitions_per_cell':5,
          'target_size_gbp':[2,4],'alphabets':['dna4','dna5'],'modes':['just_index','no_index'],
          'window_kmer_pairs':[{'kmer_size':15,'window_size':10},{'kmer_size':20,'window_size':15}],
          'total_expected_cells':64,'execution_location':'aws'},
      'eligible_stable_keys':eligible,'approved_results':list(rows.values()),
      'qualification_results':qualifications,
      'invalid_attempts':invalid,'remaining_eligible_stable_keys':sorted(set(eligible)-rows.keys())}
    output=ROOT/'research_benchmarks/paper/e2e' / ('ALL_RESULTS_TUPLES_E2E_EXTERNAL_REMOTE_'+binding['machine_id']+'.yaml')
    temporary=output.with_suffix('.yaml.tmp'); temporary.write_text(yaml.safe_dump(result,sort_keys=False,width=120)); temporary.replace(output)
    for path in (AREA/'results').glob('*/*/report.json'):
        report=json.loads(path.read_text())
        # Retain every archived output/report. Once this exact cell is accepted,
        # failed attempts no longer need their reproducible native index files.
        if report['status'] in ['validated_pending_review','invalid'] and key(report['config']) in rows:
            index=path.parent/'index'
            if index.is_dir():shutil.rmtree(index)
    print(json.dumps({'file':str(output),'approved_results':len(rows),'remaining':len(eligible)-len(rows)}),flush=True)


if __name__=='__main__': main()
