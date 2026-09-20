"""Apply idempotent benchmark-only function-region instrumentation to vendor sources."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
HEADER = r'''
#ifndef E2E_EXTERNAL_REGION_H
#define E2E_EXTERNAL_REGION_H
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
typedef struct { const char *name; uint64_t begin; } e2e_external_region;
static inline uint64_t e2e_external_now(void) {
    struct timespec stamp;
    clock_gettime(CLOCK_MONOTONIC, &stamp);
    return (uint64_t)stamp.tv_sec * 1000000000ull + stamp.tv_nsec;
}
static inline void e2e_external_region_end(e2e_external_region *region) {
    const uint64_t end = e2e_external_now();
    char line[256];
    const int count = snprintf(line, sizeof(line), "%s\t%llu\t%llu\t%ld\n",
        region->name, (unsigned long long)region->begin,
        (unsigned long long)end, (long)getpid());
    if (count > 0 && count < (int)sizeof(line)) (void)write(198, line, count);
}
#define E2E_EXTERNAL_REGION(name) e2e_external_region e2e_scope \
    __attribute__((cleanup(e2e_external_region_end))) = {name, e2e_external_now()}
#endif
'''


def instrument(path, function, name):
    source = path.read_text()
    marker = f'E2E_EXTERNAL_REGION("{name}")'
    if marker in source:
        return
    matches = list(re.finditer(r'\b' + re.escape(function) + r'\s*\([^;{}]*\)\s*\{', source))
    if len(matches) != 1:
        raise ValueError(f"Expected one definition of {function}: {path}: {len(matches)}")
    offset = matches[0].end()
    source = source[:offset] + '\n    ' + marker + ';\n' + source[offset:]
    if '#include "e2e_external_region.h"' not in source:
        source = '#include "e2e_external_region.h"\n' + source
    path.write_text(source)
    (path.parent / 'e2e_external_region.h').write_text(HEADER)


GO_HEADER = 'package cmd\nimport (\n "fmt"\n "golang.org/x/sys/unix"\n)\nfunc e2eExternalNow() int64 {\n var stamp unix.Timespec\n if err := unix.ClockGettime(unix.CLOCK_MONOTONIC, &stamp); err != nil { panic(err) }\n return stamp.Sec*1000000000+stamp.Nsec\n}\nfunc e2eExternalEnd(name string, begin int64) {\n end := e2eExternalNow()\n line := fmt.Sprintf("%s\\t%d\\t%d\\t%d\\n", name, begin, end, unix.Getpid())\n _, _ = unix.Write(198, []byte(line))\n}\n'


def instrument_go():
    directory=ROOT/'bench/tool_lexicmap/source/lexicmap/cmd'
    (directory/'e2e_external_regions.go').write_text(GO_HEADER)
    definitions=[('lib-index-search.go',r'func \(\w+ \*Index\) Search\([^{}]*\) [^{]*\{','mm_map_frag_core'),
                 ('lib-chaining.go',r'func \(\w+ \*Chainer\) Chain\([^{}]*\) [^{]*\{','mg_lchain_dp'),
                 ('lib-chaining2.go',r'func \(\w+ \*Chainer2\) Chain\([^{}]*\) [^{]*\{','mg_lchain_dp'),
                 ('lib-chaining3.go',r'func \(\w+ \*Chainer3\) Chain\([^{}]*\) [^{]*\{','mg_lchain_dp')]
    for filename,pattern,region in definitions:
        path=directory/filename; source=path.read_text()
        marker='defer e2eExternalEnd("tool_lexicmap.'+region+'", e2eExternalNow())'
        if marker not in source:
            matches=list(re.finditer(pattern,source))
            if len(matches)!=1: raise ValueError(f'Go definition mismatch: {filename}: {len(matches)}')
            pos=matches[0].end(); source=source[:pos]+'\n'+marker+'\n'+source[pos:]
            path.write_text(source)
    path=directory/'lib-index-search.go'; source=path.read_text()
    if 'e2eMaskBegin :=' not in source:
        needle='_kmers, _locses, err := funcMask(s, nil, true)'
        if source.count(needle)!=1: raise ValueError('LexicMap mask call changed')
        source=source.replace(needle,'e2eMaskBegin := e2eExternalNow()\n'+needle+'\ne2eExternalEnd("tool_lexicmap.mm_sketch", e2eMaskBegin)')
        begin='// 2) matching the captured k-mers in databases'
        end='// 3) chaining matches for all reference genomes, and alignment'
        if source.count(begin)!=1 or source.count(end)!=1: raise ValueError('LexicMap seed block changed')
        source=source.replace(begin,begin+'\n e2eSeedBegin := e2eExternalNow()')
        source=source.replace(end,'e2eExternalEnd("tool_lexicmap.collect_seed_hits", e2eSeedBegin)\n'+end)
        path.write_text(source)

    if 'e2eAlignBegin :=' not in source:
        needle='cigar, err = algn.Align(_qseq, _tseq)'
        if source.count(needle)!=2: raise ValueError('LexicMap alignment call count changed')
        source=source.replace(needle,'e2eAlignBegin := e2eExternalNow()\n'+needle+'\ne2eExternalEnd("tool_lexicmap.mm_align_skeleton", e2eAlignBegin)')
        path.write_text(source)


def enable_winnowmap_persisted_index():
    directory=ROOT/'bench/tool_winnowmap/source/src'
    # The MCAS path can cover the entire query without calling collect_seed_hits
    # in the outer scope. Its repeat length must start at zero in that case;
    # otherwise native MAPQ and the rl tag consume an uninitialized stack value.
    path=directory/'map.c';source=path.read_text()
    needle='\tint i, j, rep_len, qlen_sum, n_regs0, n_mini_pos;'
    source=source.replace(needle, '\tint i, j, rep_len = 0, qlen_sum, n_regs0, n_mini_pos;', 1)
    path.write_text(source)
    path=directory/'main.c';source=path.read_text()
    source=source.replace('"2aSDw:', '"2aSDd:w:')
    source=source.replace("/*else if (c == 'd') fnw = o.arg; // the above are indexing related options, except -I*/",
                          "else if (c == 'd') fnw = o.arg; // Enable native persisted-index output.")
    path.write_text(source)
    path=directory/'index.c';source=path.read_text()
    if 'static void mm_idx_load_weights(' not in source:
        begin=source.index('\t//Read kmer frequency file')
        end=source.index('\n\tkt_pipeline(',begin)
        body=source[begin:end].replace('pl.mi->','mi->')
        helper='static void mm_idx_load_weights(mm_idx_t *mi, const char *kmer_freq_filename)\n{\n\tconst int k = mi->k;\n'+body+'\n}\n\n'
        source=source[:begin]+'\tmm_idx_load_weights(pl.mi, kmer_freq_filename);\n'+source[end:]
        definition=source.index('mm_idx_t *mm_idx_gen(')
        source=source[:definition]+helper+source[definition:]
        needle='mi = mm_idx_load(r->fp.idx);'
        if source.count(needle)!=1:raise ValueError('Winnowmap index reader changed')
        source=source.replace(needle,needle+'\n\t\tif (mi) mm_idx_load_weights(mi, kmer_freq_filename);')
        path.write_text(source)


def main():
    instrument_go()
    enable_winnowmap_persisted_index()
    blast = ROOT / 'bench/tool_blastn/source/c++/src/algo/blast/core'
    for file, function, region in [
        ('blast_engine.c', 'Blast_RunPreliminarySearchWithInterrupt', 'mm_map_frag_core'),
        ('na_ungapped.c', 'BlastNaWordFinder', 'collect_seed_hits'),
        ('blast_traceback.c', 'Blast_RunTracebackSearchWithInterrupt', 'mm_align_skeleton'),
    ]:
        instrument(blast / file, function, 'tool_blastn.' + region)
    winnow = ROOT / 'bench/tool_winnowmap/source/src'
    for file, function, region in [
        ('sketch.c','mm_sketch','mm_sketch'),
        ('map.c','collect_minimizers','base_collect_minimizers'),
        ('map.c','collect_seed_hits','collect_seed_hits'),
        ('chain.c','mm_chain_dp','mg_lchain_dp'),
        ('map.c','chain_post','chain_post'),
        ('map.c','align_regs','align_regs'),
        ('align.c','mm_align_skeleton','mm_align_skeleton'),
        ('hit.c','mm_set_mapq','mm_set_mapq'),
    ]:
        instrument(winnow / file, function, 'tool_winnowmap.' + region)
    mmseqs = ROOT / 'bench/tool_mmseqs2/source/src'
    fix_mmseqs_diagonal_counter()
    parameters = mmseqs/'commons/Parameters.cpp'
    source = parameters.read_text()
    marker = 'createlinindex = combineList(createlinindex, splitsequence);'
    if marker not in source:
        needle = 'createlinindex = combineList(kmerindexdb, extractorfs);'
        if source.count(needle) != 1: raise ValueError('MMseqs2 linear index parameter list changed')
        parameters.write_text(source.replace(needle, needle+'\n    '+marker))
    instrument(mmseqs/'alignment/Matcher.cpp','Matcher::getSWResult','tool_mmseqs2.mm_align_skeleton')
    instrument(mmseqs/'prefiltering/Prefiltering.cpp','Prefiltering::runSplits','tool_mmseqs2.collect_seed_hits')
    instrument(mmseqs/'linclust/kmersearch.cpp','kmersearch','tool_mmseqs2.collect_seed_hits')


def fix_mmseqs_diagonal_counter():
    path=ROOT/'bench/tool_mmseqs2/source/src/linclust/kmersearch.cpp'
    source=path.read_text()
    if 'short prevDiagonal = kmers[i].pos;' in source:return
    source=source.replace('short prevDiagonal;', 'short prevDiagonal = kmers[i].pos;')
    needle='''            prevDiagonal = kmers[i].pos;

            cnt = (kmers[i].pos == prevDiagonal) ? cnt + 1 : 1 ;'''
    if source.count(needle)!=1:raise ValueError('MMseqs2 native diagonal counter changed')
    source=source.replace(needle,'''            cnt = (kmers[i].pos == prevDiagonal) ? cnt + 1 : 1 ;
            prevDiagonal = kmers[i].pos;''')
    path.write_text(source)


if __name__ == '__main__':
    main()
