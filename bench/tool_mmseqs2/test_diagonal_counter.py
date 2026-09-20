"""Compile the actual native aggregation function remotely against controlled seeds."""
from pathlib import Path
import platform
import subprocess
import tempfile
import json
import sys

if platform.system()!='Linux':raise RuntimeError('Remote Linux builder required')
source=(Path(__file__).parent/'source/src/linclust/kmersearch.cpp').read_text()
begin=source.index('template <int TYPE>\nvoid KmerSearch::writeResult(')
end=source.index('\ntemplate void KmerSearch::writeResult<',begin)
native=source[begin:end]
prefix=r'''
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>
#include <cassert>
#include <iostream>
using DBKeyType=uint32_t;
#define SIZE_T_MAX SIZE_MAX
#define BIT_CHECK(x,b) (((x)>>(b))&1)
#define BIT_CLEAR(x,b) ((x)&~(uint64_t(1)<<(b)))
struct Parameters {static constexpr int DBTYPE_NUCLEOTIDES=1;};
template<typename T,bool A,bool B> struct KmerPosition {uint64_t kmer;DBKeyType id;T pos;};
struct hit_t {DBKeyType seqId;int prefScore;short diagonal;};
struct QueryMatcher {static int prefilterHitToBuffer(char *s,const hit_t &h) {
    return std::sprintf(s,"%u\t%d\t%d\n",h.seqId,h.prefScore,h.diagonal);
}};
struct DBWriter {std::vector<std::pair<DBKeyType,std::string>> rows;
    void writeData(const char *p,size_t n,DBKeyType key,int) {rows.emplace_back(key,std::string(p,n));}
};
struct KmerSearch {template<int TYPE> static void writeResult(DBWriter &,KmerPosition<short,false,true> *,size_t);};
'''
suffix=r'''
int main() {
    constexpr uint64_t forward=uint64_t(1)<<63;
    // Three consistent forward seeds followed by one weak reverse seed.
    // Then three consistent reverse seeds followed by one weak forward seed.
    KmerPosition<short,false,true> seeds[]={
        {forward,42,-8000},{forward,42,-8000},{forward,42,-8000},{0,42,500},
        {1,43,-6000},{1,43,-6000},{1,43,-6000},{forward|1,43,700}};
    DBWriter writer;KmerSearch::writeResult<1>(writer,seeds,8);
    if(writer.rows.size()!=2)return 1;
    if(writer.rows[0].first!=0 || writer.rows[0].second!="42\t4\t-8000\n")return 2;
    if(writer.rows[1].first!=1 || writer.rows[1].second!="43\t-4\t-6000\n")return 3;
    std::cout << "Native MMseqs2 strongest-diagonal regression: forward and reverse passed\n";
}
'''
with tempfile.TemporaryDirectory(prefix='mmseqs-native-diagonal-') as temporary:
    root=Path(temporary);cpp=root/'check.cpp';binary=root/'check'
    old=native.replace('''            cnt = (kmers[i].pos == prevDiagonal) ? cnt + 1 : 1 ;
            prevDiagonal = kmers[i].pos;''','''            prevDiagonal = kmers[i].pos;
            cnt = (kmers[i].pos == prevDiagonal) ? cnt + 1 : 1 ;''')
    if old==native:raise ValueError('Expected corrected native counter')
    cpp.write_text(prefix+old+suffix)
    subprocess.run(['clang++','-std=c++17','-O2',str(cpp),'-o',str(binary)],check=True,timeout=60)
    if subprocess.run([str(binary)],timeout=30).returncode==0:raise AssertionError('Original counter unexpectedly passed regression')
    cpp.write_text(prefix+native+suffix)
    subprocess.run(['clang++','-std=c++17','-O2',str(cpp),'-o',str(binary)],check=True,timeout=60)
    subprocess.run([str(binary)],check=True,timeout=30)

# On the pilot host, replay the real previously failing query against its
# completed native index. This is an untimed regression, never a matrix row.
repository=Path(__file__).resolve().parents[2]
reports=sorted((repository/'bench/e2e_external/results/mmseqs2_M2_D4_K15_W10_just_index').glob('*/report.json'),reverse=True)
for report_path in reports:
    report=json.loads(report_path.read_text());index=report_path.parent/'index/group0'
    if 'Wrong occurrence q116:' not in report.get('error','') or not (index/'target.linidx').is_file():continue
    sys.path.insert(0,str(repository/'bench/e2e_external'))
    from campaign import read_fasta,validate
    import single_index_adapter
    import adapter
    fixture=repository/'bench/e2e_external/fixtures'/report['config']['dataset']
    query_sequence=read_fasta(fixture/'queries.fasta')['q116']
    fields=(fixture/'truth.tsv').read_text().splitlines()[116].split('\t')
    truth={'start':int(fields[1]),'end':int(fields[2]),'reverse':bool(int(fields[3])),
           'window_start':int(fields[5]),'window':fields[6]}
    with tempfile.TemporaryDirectory(prefix='mmseqs-q116-regression-') as temporary:
        work=Path(temporary);query=work/'query.fasta';query.write_text('>q116\n'+query_sequence+'\n')
        def run(argv):
            result=subprocess.run(list(map(str,argv)),cwd=repository,capture_output=True,text=True,timeout=90)
            if result.returncode:raise RuntimeError(result.stderr[-3000:]+result.stdout[-3000:])
        output=single_index_adapter.align(repository,report['config'],fixture,index,query,work,run)
        output.write_text(''.join('\t'.join(line.split('\t')[:8])+'\n' for line in output.read_text().splitlines()))
        rows=[{**row,'query':0} for row in adapter.parse(output)]
        validate(rows,[truth],{'q0':query_sequence},1,False)
        print('Native MMseqs2 q116 integration regression: full occurrence, strand and gapped replay passed')
    break
