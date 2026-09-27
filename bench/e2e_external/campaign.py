#!/usr/bin/env python3
"""External-only campaign: cold processes, bounded runs and occurrence audits."""
import collections
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[2]
AREA=ROOT/'bench/e2e_external'
SPLIT_BP=2000000352
TOOLS={'blastn','mmseqs2','winnowmap','lexicmap'}


def digest(path):
    value=hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda:stream.read(1024*1024),b''): value.update(block)
    return value.hexdigest()


def atomic_json(path,value):
    temporary=path.with_suffix(path.suffix+'.tmp')
    with temporary.open('w') as stream:
        json.dump(value,stream,indent=2); stream.write('\n'); stream.flush(); os.fsync(stream.fileno())
    os.replace(temporary,path)


def read_fasta(path):
    records={}; name=None
    for line in path.read_text().splitlines():
        if line.startswith('>'): name=line[1:].split()[0]; records[name]=''
        else: records[name]+=line.upper()
    return records


def reverse_complement(sequence):
    return sequence.translate(str.maketrans('ACGTN','TGCAN'))[::-1]


def memory_snapshot():
    return {'swaps':Path('/proc/swaps').read_text(),
        'oom_kill':next(int(line.split()[1]) for line in Path('/proc/vmstat').read_text().splitlines() if line.startswith('oom_kill ')),
        'zram_mm_stat':{p.parent.name:p.read_text().strip() for p in Path('/sys/block').glob('zram*/mm_stat')}}


def validate(rows,truth,queries,count,mapq_required):
    counts=[0]*count; maximum_offset=0; verified=[]; edit_distance_checks=0; encoded_reference_bases=0
    for row in rows:
        ordinal=row['query']
        if ordinal<0 or ordinal>=count: raise ValueError('Unexpected query ordinal')
        counts[ordinal]+=1
        expected=truth[ordinal]
        if not re.fullmatch(r'ref_part[0-9]+',row['target']): raise ValueError('Unknown target identifier')
        offset=int(row['target'].removeprefix('ref_part'))*SPLIT_BP
        qs,qe=row['qfirst'],row['qlast']; ts,te=row['tfirst'],row['tlast']
        start=offset+min(ts,te)-1; end=offset+max(ts,te)
        reverse=(qs>qe)!=(ts>te)
        error=max(abs(start-expected['start']),abs(end-expected['end']))
        maximum_offset=max(maximum_offset,error)
        if error>=15 or reverse!=expected['reverse']: raise ValueError(f'Wrong occurrence q{ordinal}: {start}:{end} reverse={reverse}; expected {expected["start"]}:{expected["end"]} reverse={expected["reverse"]}')
        query=queries['q'+str(ordinal)]
        if min(qs,qe)<1 or max(qs,qe)>len(query): raise ValueError('Query coordinates out of bounds')
        query=query[min(qs,qe)-1:max(qs,qe)]
        if qs>qe: query=reverse_complement(query)
        window_start=expected['window_start']; window=expected['window']
        target=window[start-window_start:end-window_start]
        encoding=row.get('reference_encoding')
        if encoding=='lexicmap_two_bit':
            # Native storage maps N to A before any reverse complementation.
            # Restrict this qualification to reference output; queries stay exact.
            encoded_reference_bases+=target.count('N')
            target=target.replace('N','A')
        elif encoding is not None:
            raise ValueError('Unknown native reference encoding')
        if ts>te: target=reverse_complement(target)
        if row.get('qaln') is not None:
            qaln=row['qaln'].upper(); taln=row['taln'].upper()
            if len(qaln)!=len(taln) or not qaln or any(a==b=='-' for a,b in zip(qaln,taln)):
                raise ValueError('Malformed gapped alignment')
            if qaln.replace('-','')!=query or taln.replace('-','')!=target:
                raise ValueError(f'Gapped alignment replay mismatch for q{ordinal}')
        else:
            cigar=row.get('cigar',''); operations=re.findall(r'(\d+)([MIDNSHP=X])',cigar or '')
            if not operations or ''.join(n+op for n,op in operations)!=cigar:
                raise ValueError('Missing or malformed CIGAR')
            qpos=tpos=edit_distance=0
            for size,operation in operations:
                size=int(size)
                if size<=0: raise ValueError('Zero CIGAR operation')
                if operation in 'M=X':
                    left=query[qpos:qpos+size];right=target[tpos:tpos+size]
                    if operation=='=' and left!=right: raise ValueError('CIGAR equality mismatch')
                    if operation=='X' and any(a==b for a,b in zip(left,right)):raise ValueError('CIGAR mismatch operation contains equality')
                    # Native NM counts any ambiguous aligned base as an edit,
                    # including N/N, and includes inserted/deleted bases.
                    edit_distance+=sum(a!=b or a=='N' or b=='N' for a,b in zip(left,right))
                    qpos+=size; tpos+=size
                elif operation=='I': qpos+=size;edit_distance+=size
                elif operation=='D': tpos+=size;edit_distance+=size
                elif operation=='N': tpos+=size
                else: raise ValueError('Unexpected clip/pad in aligned PAF CIGAR')
                if qpos>len(query) or tpos>len(target): raise ValueError('CIGAR replay overflow')
            if qpos!=len(query) or tpos!=len(target): raise ValueError('CIGAR replay extent mismatch')
            if row.get('edit_distance')!=edit_distance:raise ValueError(f'Native NM differs from CIGAR/base replay for q{ordinal}')
            edit_distance_checks+=1
        if mapq_required and (not isinstance(row['mapq'],int) or not 0<=row['mapq']<=255):
            raise ValueError('Missing native MAPQ')
        verified.append({'query':ordinal,'start':start,'end':end,'reverse':reverse})
    if counts!=[1]*count: raise ValueError(f'Expected one occurrence per query; count distribution {dict(collections.Counter(counts))}')
    return {'valid':True,'query_count':count,'match_counts':counts,'max_position_error_bp':maximum_offset,
            'occurrences':sorted(verified,key=lambda row:row['query']),
            'gapped_replay_count':count,'native_edit_distance_checked_count':edit_distance_checks,
            'native_encoded_reference_bases_checked':encoded_reference_bases,
            'mapq_present_count':count if mapq_required else None}


def interval_union(intervals):
    total=0; previous_begin=previous_end=None
    for begin,end in sorted(intervals):
        if begin<0 or end<begin: raise ValueError('Invalid interval')
        if previous_end is None: previous_begin,previous_end=begin,end
        elif begin<=previous_end: previous_end=max(previous_end,end)
        else: total+=previous_end-previous_begin; previous_begin,previous_end=begin,end
    if previous_end is not None: total+=previous_end-previous_begin
    return total


class Commands:
    def __init__(self,work,deadline):
        self.work=work; self.deadline=deadline; self.records=[]; self.intervals=work/'intervals.tsv'

    def __call__(self,command,stdout=None):
        command=list(map(str,command))
        binary=Path(command[0]).resolve()
        allowed=[ROOT/('bench/tool_'+tool+'/bin') for tool in TOOLS]
        allowed.append(AREA/'build')
        if not any(binary.is_relative_to(path) for path in allowed): raise ValueError('Non-external executable rejected')
        identifier=len(self.records)
        with self.intervals.open('ab',buffering=0) as intervals, (self.work/f'command_{identifier}.stderr').open('wb') as stderr, Path(stdout or self.work/f'command_{identifier}.stdout').open('wb') as output:
            os.dup2(intervals.fileno(),198,inheritable=True)
            def prepare():
                os.nice(-20)
            begin=time.monotonic_ns()
            process=subprocess.Popen(command,cwd=ROOT,stdout=output,stderr=stderr,
                pass_fds=(198,),preexec_fn=prepare,start_new_session=True)
            os.close(198)
            try:
                process.wait(timeout=max(0.1,self.deadline-time.monotonic()-5))
            except subprocess.TimeoutExpired:
                os.killpg(process.pid,signal.SIGTERM)
                try: process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid,signal.SIGKILL); process.wait()
                self.records.append({'argv':command,'pid':process.pid,'begin_ns':begin,
                    'end_ns':time.monotonic_ns(),'exit_code':process.returncode,'timeout':True})
                atomic_json(self.work/'commands.json',self.records)
                raise TimeoutError('External invocation reached its wall-time bound')
            end=time.monotonic_ns()
            self.records.append({'argv':command,'pid':process.pid,'begin_ns':begin,'end_ns':end,'exit_code':process.returncode})
            atomic_json(self.work/'commands.json',self.records)
            if process.returncode: raise RuntimeError(f'{binary.name} exited {process.returncode}: '+(self.work/f'command_{identifier}.stderr').read_text(errors='replace')[-3000:])


def measure_intervals(path,begin,end,tool):
    regions=collections.defaultdict(list)
    for line in path.read_text().splitlines():
        name,start,finish,pid=line.split('\t'); start=int(start); finish=int(finish)
        if not name.startswith('tool_'+tool+'.') or not begin<=start<=finish<=end:
            raise ValueError('Interval origin or containment failure')
        regions[name].append((start,finish))
    alignment=regions.get('tool_'+tool+'.mm_align_skeleton',[])
    mapq=regions.get('tool_'+tool+'.mm_set_mapq',[])
    if not alignment: raise ValueError('No physical alignment intervals recorded')
    if tool=='winnowmap' and not mapq: raise ValueError('No physical MAPQ intervals recorded')
    return {'regions_ns':{name:interval_union(values) for name,values in regions.items()},
            'cigar_or_mapq_union_ns':interval_union(alignment+mapq),
            'interval_counts':{name:len(values) for name,values in regions.items()}}


def main(config_path):
    config=json.loads(Path(config_path).read_text()); tool=config['tool']
    if config['benchmark']!='e2e_external' or tool not in TOOLS: raise ValueError('External-only configuration required')
    if config['mode'] not in ['just_index','no_index'] or config['query_ladder']!=[2,32,1024] or config['repetitions']!=5:
        raise ValueError('Campaign mode, q ladder or repetition contract violated')
    frozen={'fragment_count':1024,'fragment_length_mean':20000,'fragment_length_stddev':500,
            'substitution_rate':0.001,'insertion_rate':0.0005,'deletion_rate':0.0005,
            'insertion_length_mean':1.0,'deletion_length_mean':1.0,'reverse_complement_rate':0.5,
            'minimap2_split_bp':SPLIT_BP,'cigar_projection':True,'mapq_projection':True}
    if any(config['data'].get(k)!=v for k,v in frozen.items()) or config['threads']!=4:
        raise ValueError('Configuration diverges from the frozen external fixture contract')
    deadline=time.monotonic()+min(config['invocation_timeout_seconds'],43200)
    module_spec=importlib.util.spec_from_file_location(tool,ROOT/('bench/tool_'+tool+'/adapter.py'))
    adapter=importlib.util.module_from_spec(module_spec); module_spec.loader.exec_module(adapter)
    label=Path(config_path).stem
    result=AREA/'results'/label/str(time.time_ns()); result.mkdir(parents=True)
    report={'schema':'bioseq.external-e2e-raw','version':1,'status':'running','config':config,
            'config_sha256':digest(config_path),'tool_source':json.loads((ROOT/('bench/tool_'+tool+'/source_lock.json')).read_text()),
            'host':{'hostname':platform.node(),'architecture':platform.machine()},
            'host_binding':json.loads((AREA/'host_binding.json').read_text()),
            'capabilities':adapter.CAPABILITIES,'comments':adapter.COMMENTS,'rungs':[]}
    report['reference_layout']={'input_records':1 if config['data']['reference_length']<=SPLIT_BP else 2,
        'reason':'Native signed 32-bit alignment coordinates require records below 2^31 bases; 2 Gbp remains a single sequence.',
        'tool_preparation': '28-bit LexicMap input partitioning with query-length overlap' if tool=='lexicmap' else
                            'Last-resort signed-16-bit FASTA records distributed across native linear indices to preserve overlapping candidates' if tool=='mmseqs2' else None}
    report['artifacts']={str(path.relative_to(ROOT)):digest(path)
        for path in [AREA/'build/e2e_external',AREA/'index.cppm',AREA/'campaign.py',
                     ROOT/('bench/tool_'+tool+'/adapter.py'),ROOT/('bench/tool_'+tool+'/build.sh')]
        + sorted((ROOT/('bench/tool_'+tool+'/bin')).glob('*')) if path.is_file()}
    report['artifacts'].update({str(path.relative_to(ROOT)):digest(path)
        for path in (ROOT/('bench/tool_'+tool)).glob('*.py')})
    matrix=adapter.matrix_path(ROOT,config) if tool=='mmseqs2' else ROOT/('bench/tool_'+tool+'/nucleotide_baseline.out')
    if matrix.exists():report['artifacts'][str(matrix.relative_to(ROOT))]=digest(matrix)
    receipt=AREA/'.state'/(tool+'_build_receipt.json')
    if receipt.exists():
        build=json.loads(receipt.read_text());seal=build.pop('seal_sha256')
        if hashlib.sha256(json.dumps(build,sort_keys=True,separators=(',',':')).encode()).hexdigest()!=seal:
            raise ValueError('Build receipt seal mismatch')
        if build['build_script_sha256']!=digest(ROOT/('bench/tool_'+tool+'/build.sh')):
            raise ValueError('Build recipe changed; rebuild this external tool before measuring')
        if build['source']['source_manifest_sha256']!=digest(ROOT/('bench/tool_'+tool+'/source_manifest.json')):
            raise ValueError('Vendor source changed; rebuild this external tool before measuring')
        for filename,expected_hash in build['artifacts'].items():
            if digest(ROOT/filename)!=expected_hash:raise ValueError('Built executable identity changed')
        report['build_receipt']={**build,'seal_sha256':seal}
    report['host']['cpu']=subprocess.check_output(['lscpu','--json'],text=True)
    report['host']['memory']=Path('/proc/meminfo').read_text()
    report['host']['swap']=Path('/proc/swaps').read_text()
    report['host']['zram']=subprocess.check_output(['zramctl','--bytes'],text=True)
    report['host']['clang']=subprocess.check_output(['clang','--version'],text=True)
    report_path=result/'report.json'; atomic_json(report_path,report)
    fixture=AREA/'fixtures'/config['dataset']; fixture.mkdir(parents=True,exist_ok=True)
    try:
        marker=fixture/'identity.json'
        expected={'alphabet':config['data']['alphabet'],'length':config['data']['reference_length'],'seed':config['data']['seed'],
                  'generator_sha256':digest(AREA/'index.cppm')}
        if not marker.exists():
            preparation=result/'fixture'; preparation.mkdir()
            Commands(preparation,deadline)([AREA/'build/e2e_external','fixture',expected['alphabet'],expected['length'],expected['seed'],fixture])
            atomic_json(marker,{**expected,'target_sha256':digest(fixture/'target.fasta'),
                               'queries_sha256':digest(fixture/'queries.fasta'),'truth_sha256':digest(fixture/'truth.tsv')})
        identity=json.loads(marker.read_text())
        if any(identity.get(k)!=v for k,v in expected.items()): raise ValueError('Fixture source identity changed')
        for filename,key in [('target.fasta','target_sha256'),('queries.fasta','queries_sha256'),('truth.tsv','truth_sha256')]:
            if digest(fixture/filename)!=identity[key]: raise ValueError('Fixture content changed')
        report['fixture']=identity
        queries=read_fasta(fixture/'queries.fasta')
        truth=[]
        for line in (fixture/'truth.tsv').read_text().splitlines():
            ordinal,start,end,reverse,length,window_start,window=line.split('\t')
            if int(ordinal)!=len(truth): raise ValueError('Truth ordinal gap')
            truth.append({'start':int(start),'end':int(end),'reverse':bool(int(reverse)),
                          'window_start':int(window_start),'window':window})
        index=result/'index'; index.mkdir()
        if config['mode']=='just_index':
            setup=result/'setup'; setup.mkdir()
            setup_commands=Commands(setup,deadline)
            report['setup_provenance']=adapter.setup(ROOT,config,fixture,index,setup_commands)
            report['untimed_index_setup']=setup_commands.records
        for query_count in config['query_ladder']:
            rung={'query_count':query_count,'repeats':[]}; report['rungs'].append(rung)
            query=result/f'q{query_count}.fasta'
            query.write_text(''.join('>q'+str(i)+'\n'+queries['q'+str(i)]+'\n' for i in range(query_count)))
            for repeat in range(5 if query_count==1024 else 1):
                work=result/f'q{query_count}_repeat{repeat}'; work.mkdir()
                if config['mode']=='no_index':
                    shutil.rmtree(index); index.mkdir()
                commands=Commands(work,deadline)
                memory_before=memory_snapshot()
                begin=time.monotonic_ns()
                index_metadata=None
                if config['mode']=='no_index': index_metadata=adapter.setup(ROOT,config,fixture,index,commands)
                output=adapter.align(ROOT,config,fixture,index,query,work,commands)
                end=time.monotonic_ns()
                memory_after=memory_snapshot()
                observation={'repeat':repeat,'a_ns':end-begin,'commands':commands.records,'output_sha256':digest(output),
                    'memory_before':memory_before,'memory_after':memory_after,'index_preparation_metadata':index_metadata}
                rung['repeats'].append(observation)
                atomic_json(report_path,report)
                observation['audit']=validate(adapter.parse(output),truth,queries,query_count,adapter.CAPABILITIES['mapq'])
                phases=measure_intervals(commands.intervals,begin,end,tool)
                observation.update(phases)
                observation['b_ns']=observation['a_ns']-phases['cigar_or_mapq_union_ns']
                observation['valid']=True
                atomic_json(report_path,report)
                print(json.dumps({'dataset':config['dataset'],'tool':tool,'mode':config['mode'],
                      'q':query_count,'repeat':repeat,'a_ns':observation['a_ns'],'valid':True}),flush=True)
            rung['passed']=True
        final=report['rungs'][-1]['repeats']
        report['metrics']={'unit':'ns','p50_a':sorted(r['a_ns'] for r in final)[2],
                           'p50_b':sorted(r['b_ns'] for r in final)[2]}
        report['run_count']=5; report['status']='validated_pending_review'
        for filename,expected_hash in report['artifacts'].items():
            if digest(ROOT/filename)!=expected_hash: raise ValueError('Source or executable changed during measurement')
    except Exception as error:
        report['status']='invalid'; report['error']=str(error)
        report['memory_at_failure']=memory_snapshot()
        print('INVALID: '+str(error),flush=True)
    finally:
        atomic_json(report_path,report)
    return 0 if report['status']=='validated_pending_review' else 1


if __name__=='__main__':
    raise SystemExit(main(sys.argv[1]))
