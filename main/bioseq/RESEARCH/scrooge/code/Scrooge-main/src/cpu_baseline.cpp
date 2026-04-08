#include "util.hpp"
#include <string>
#include <cstring>
#include <vector>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <thread>
#include <atomic>
#include <set>
#include <omp.h>
#include <cassert>
#include "filesystem.hpp"

#include "../baseline_algorithms/ksw2/ksw2.h"
#include "../baseline_algorithms/edlib/edlib.h"
#include "../baseline_algorithms/wfa_lm/wfa_lm.hpp"
#include "../baseline_algorithms/gact/gact.hpp"
#include "genasm_cpu.hpp"

#ifdef LIB_WFA
#include "gap_affine/affine_wavefront_align.h"
#include "gap_affine/affine_wavefront.h"
#endif

using namespace std;


struct AffineGapCosts {
    int match_bonus; //positive
    int mismatch_cost; //positive
    int gap_open_cost; //positive
    int gap_extend_cost; //positive
};

bool enable_log = true;

Alignment_t extz_to_alignment(ksw_extz_t ez, string reference, string read){
    for (auto & c: reference) c = toupper(c);
    for (auto & c: read) c = toupper(c);

    stringstream ss;
    long long edit_distance = 0;
    long long i = 0;
    long long j = 0;

    for(int edit = 0; edit < ez.n_cigar; edit++){
        int edit_count = ez.cigar[edit]>>4;
        char edit_type = "MID"[ez.cigar[edit]&0xf];
        if(edit_type == 'M'){
            char last_edit_type = reference[i] == read[j] ? '=' : 'X';
            int last_edit_count = 1;
            i++;
            j++;
            for(int eq_i = 1; eq_i < edit_count; eq_i++){
                if(reference[i] == read[j]){
                    edit_type = '=';
                }
                else{
                    edit_type = 'X';
                    edit_distance++;
                }
                if(edit_type != last_edit_type){
                    ss << last_edit_count;
                    ss << last_edit_type;
                    last_edit_count = 0;
                    last_edit_type = edit_type;
                }
                i++;
                j++;
                last_edit_count++;
            }
            ss << last_edit_count;
            ss << last_edit_type;
        }
        if(edit_type == 'I'){
            j+=edit_count;
            edit_distance+=edit_count;
            ss << edit_count;
            ss << edit_type;
        }
        if(edit_type == 'D'){
            i+=edit_count;
            edit_distance+=edit_count;
            ss << edit_count;
            ss << edit_type;
        }
    }

    Alignment_t res;
    res.cigar = ss.str();
    res.edit_distance = edit_distance;
    return res;
}

vector<Alignment_t> benchmark_ksw2_extz(Genome_t &reference, vector<Read_t> &reads, AffineGapCosts agc, int threads){
    if(enable_log) cerr << "benchmark_ksw2_extz()..." << endl;
    int n = reference.content.size();

    int8_t a = agc.match_bonus;
    int8_t b = -agc.mismatch_cost;
    int8_t score_matrix[25] = { a,b,b,b,0, b,a,b,b,0, b,b,a,b,0, b,b,b,a,0, 0,0,0,0,0 };
    int gap_open = agc.gap_open_cost;
    int gap_extend = agc.gap_extend_cost;

    uint8_t c[256];
	memset(c, 4, 256);
	c['A'] = c['a'] = 0; c['C'] = c['c'] = 1;
	c['G'] = c['g'] = 2; c['T'] = c['t'] = 3;

    if(enable_log) cerr << "building twobit reference..." << endl;
	uint8_t *twobit_reference = (uint8_t*)malloc(reference.content.size());
    for(int i = 0; i < n; i++) twobit_reference[i] = c[(uint8_t)reference.content[i]];

    if(enable_log) cerr << "building twobit reads..." << endl;
    uint8_t **twobit_reads = (uint8_t**)malloc(reads.size()*sizeof(uint8_t*));
    for(int read_idx = 0; read_idx < reads.size(); read_idx++){
        int m = reads[read_idx].content.size();
        twobit_reads[read_idx] = (uint8_t*)malloc(m);
        for(int i = 0; i < m; i++) twobit_reads[read_idx][i] = c[(uint8_t)reads[read_idx].content[i]];
    }

    if(enable_log) cerr << "building twobit pairs..." << endl;
    size_t num_pairs = 0;
    for(Read_t &read : reads){
        num_pairs += read.locations.size();
    }

    struct ksw2_input_pair {
        uint8_t *text;
        uint8_t *pattern;
        int n;
        int m;
        int band_width;
    };
    vector<ksw2_input_pair> inputs;
    size_t pair_idx = 0;
    for(size_t i = 0; i < reads.size(); i++){
        for(CandidateLocation_t &location : reads[i].locations){
            ksw2_input_pair input;
            input.pattern = twobit_reads[i];
            input.text = twobit_reference + location.start_in_reference;
            input.m = reads[i].content.size();
            input.n = min((int)(1.15*input.m), n - (int)location.start_in_reference);
            input.band_width = (int)(0.15*input.m);
            inputs.push_back(input);
            pair_idx++;
        }
    }

    vector<ksw_extz_t> ezs(num_pairs);
    if(enable_log) cerr << "running algorithm..." << endl;
    auto workload = [&](){
        omp_set_num_threads(threads);
        #pragma omp parallel
        {
            #pragma omp for
            for(long long pair_idx = 0; pair_idx < num_pairs; pair_idx++){
                ksw_extz_t ez;
                memset(&ez, 0, sizeof(ksw_extz_t));
                ksw_extz(NULL, inputs[pair_idx].m, inputs[pair_idx].pattern, inputs[pair_idx].n, inputs[pair_idx].text, 5, score_matrix,
                    gap_open, gap_extend, inputs[pair_idx].band_width, -1, KSW_EZ_EXTZ_ONLY, &ez); //unsure if the KSW_EZ_EXTZ_ONLY is correct
                ezs[pair_idx] = ez;
            }
        }
    };
    long long ns = measure_ns(workload);

    double aligns_per_second = 1.0*num_pairs * 1000*1000*1000 / ns;
    cout << fixed << setprecision(2);
    cout << "ksw2_extz: " << aligns_per_second << " aligns/second" << endl;

    vector<Alignment_t> alignments;
    pair_idx = 0;
    for(size_t i = 0; i < reads.size(); i++){
        for(CandidateLocation_t &location : reads[i].locations){
            string ref = reference.content.substr(location.start_in_reference, reads[i].content.size()*2);
            Alignment_t al = extz_to_alignment(ezs[pair_idx], ref, reads[i].content);
            alignments.push_back(al);
            free(ezs[pair_idx].cigar);

            pair_idx++;
        }
    }

	for(int read_idx = 0; read_idx < reads.size(); read_idx++)
        free(twobit_reads[read_idx]);
    free(twobit_reads);
    free(twobit_reference);

    return alignments;
}

vector<Alignment_t> benchmark_ksw2_extz2_sse(Genome_t &reference, vector<Read_t> &reads, AffineGapCosts agc, int threads){
    if(enable_log) cerr << "benchmark_ksw2_extz2_sse()..." << endl;
    int n = reference.content.size();

    int8_t a = agc.match_bonus;
    int8_t b = -agc.mismatch_cost;
    int8_t score_matrix[25] = { a,b,b,b,0, b,a,b,b,0, b,b,a,b,0, b,b,b,a,0, 0,0,0,0,0 };
    int gap_open = agc.gap_open_cost;
    int gap_extend = agc.gap_extend_cost;

    int end_bonus = 0;

    uint8_t c[256];
	memset(c, 4, 256);
	c['A'] = c['a'] = 0; c['C'] = c['c'] = 1;
	c['G'] = c['g'] = 2; c['T'] = c['t'] = 3;

    if(enable_log) cerr << "building twobit reference..." << endl;
	uint8_t *twobit_reference = (uint8_t*)malloc(reference.content.size());
    for(int i = 0; i < n; i++) twobit_reference[i] = c[(uint8_t)reference.content[i]];

    if(enable_log) cerr << "building twobit reads..." << endl;
    uint8_t **twobit_reads = (uint8_t**)malloc(reads.size()*sizeof(uint8_t*));
    for(int read_idx = 0; read_idx < reads.size(); read_idx++){
        int m = reads[read_idx].content.size();
        twobit_reads[read_idx] = (uint8_t*)malloc(m);
        for(int i = 0; i < m; i++) twobit_reads[read_idx][i] = c[(uint8_t)reads[read_idx].content[i]];
    }

    if(enable_log) cerr << "building twobit pairs..." << endl;
    size_t num_pairs = 0;
    for(Read_t &read : reads){
        num_pairs += read.locations.size();
    }

    struct ksw2_input_pair {
        uint8_t *text;
        uint8_t *pattern;
        int n;
        int m;
        int band_width;
    };
    vector<ksw2_input_pair> inputs;
    size_t pair_idx = 0;
    for(size_t i = 0; i < reads.size(); i++){
        for(CandidateLocation_t &location : reads[i].locations){
            ksw2_input_pair input;
            input.pattern = twobit_reads[i];
            input.text = twobit_reference + location.start_in_reference;
            input.m = reads[i].content.size();
            input.n = min((int)(1.15*input.m), n - (int)location.start_in_reference);
            input.band_width = (int)(0.15*input.m);
            inputs.push_back(input);
            pair_idx++;
        }
    }
    
    vector<ksw_extz_t> ezs(num_pairs);
    if(enable_log) cerr << "running algorithm..." << endl;
    auto workload = [&](){
        omp_set_num_threads(threads);
        #pragma omp parallel
        {
            #pragma omp for
            for(long long pair_idx = 0; pair_idx < num_pairs; pair_idx++){
                ksw_extz_t ez;
                memset(&ez, 0, sizeof(ksw_extz_t));
                ksw_extz2_sse(NULL, inputs[pair_idx].m, inputs[pair_idx].pattern, inputs[pair_idx].n, inputs[pair_idx].text, 5, score_matrix,
                    gap_open, gap_extend, inputs[pair_idx].band_width, -1, end_bonus, KSW_EZ_EXTZ_ONLY, &ez); //unsure if the KSW_EZ_EXTZ_ONLY is correct
                ezs[pair_idx] = ez;
            }
        }
    };
    long long ns = measure_ns(workload);

    double aligns_per_second = 1.0*num_pairs * 1000*1000*1000 / ns;
    cout << fixed << setprecision(2);
    cout << "ksw2_extz2_sse: " << aligns_per_second << " aligns/second" << endl;

    vector<Alignment_t> alignments;
    pair_idx = 0;
    for(size_t i = 0; i < reads.size(); i++){
        for(CandidateLocation_t &location : reads[i].locations){
            string ref = reference.content.substr(location.start_in_reference, reads[i].content.size()*2);
            Alignment_t al = extz_to_alignment(ezs[pair_idx], ref, reads[i].content);
            alignments.push_back(al);
            free(ezs[pair_idx].cigar);

            pair_idx++;
        }
    }

	for(int read_idx = 0; read_idx < reads.size(); read_idx++)
        free(twobit_reads[read_idx]);
    free(twobit_reads);
    free(twobit_reference);

    return alignments;
}

Alignment_t edlib_to_alignment(EdlibAlignResult ed){
    Alignment_t res;
    if(ed.status != EDLIB_STATUS_OK || ed.editDistance==-1){
        res.edit_distance = -1;
        res.cigar = "";
    }
    else{
        res.edit_distance = ed.editDistance;
        res.cigar = edlibAlignmentToCigar(ed.alignment, ed.alignmentLength, EdlibCigarFormat::EDLIB_CIGAR_EXTENDED);
    }
    return res;
}

vector<Alignment_t> benchmark_edlib(Genome_t &reference, vector<Read_t> &reads, int threads){
    if(enable_log) cerr << "benchmark_edlib()..." << endl;
    long long n = reference.content.size();

    for (auto & c: reference.content) c = toupper(c);
    for(Read_t &read: reads)
        for (auto & c: read.content) c = toupper(c);

    if(enable_log) cerr << "building input pairs..." << endl;
    size_t num_pairs = 0;
    for(Read_t &read : reads){
        num_pairs += read.locations.size();
    }

    struct edlib_input_pair{
        char *text;
        char *pattern;
        int n;
        int m;
        int k; //edit distance threshold, -1 for automatic
    };
    vector<edlib_input_pair> inputs;

    size_t pair_idx = 0;
    for(size_t i = 0; i < reads.size(); i++){
        for(CandidateLocation_t &location : reads[i].locations){
            edlib_input_pair input;
            input.pattern = (char *)reads[i].content.c_str();
            input.text = (char *)reference.content.c_str() + location.start_in_reference;
            input.m = reads[i].content.size();
            input.n = min((long long)(1.15*input.m), n - (int)location.start_in_reference);
            //input.k = -1;
            input.k = (int)(0.15*input.m);
            //input.k = (int)(0.50*input.m);
            inputs.push_back(input);
            pair_idx++;
        }
    }

    if(enable_log) cerr << "running edlib..." << endl;
    vector<EdlibAlignResult> ed_ress(num_pairs);

    auto workload = [&](){
        omp_set_num_threads(threads);
        #pragma omp parallel for
        for(long long pair_idx = 0; pair_idx < num_pairs; pair_idx++){
                EdlibAlignConfig conf;
                conf.k = inputs[pair_idx].k;
                conf.mode = EDLIB_MODE_SHW; //semi global on the right, left fixed
                conf.task = EDLIB_TASK_PATH; //calculate with traceback
                conf.additionalEqualities = NULL;
                conf.additionalEqualitiesLength = 0;

                EdlibAlignResult res = edlibAlign(inputs[pair_idx].pattern, inputs[pair_idx].m,
                                                inputs[pair_idx].text, inputs[pair_idx].n, conf);
                if(res.status != EDLIB_STATUS_OK || res.editDistance==-1){
                    //if(enable_log) cerr << "edlib alignment failed pair_idx=" << pair_idx << endl;
                }
                else{
                    //if(enable_log) cerr << "edlib alignment successfull pair_idx=" << pair_idx << endl;
                    //optimization_blocker = res;
                }
                //edlibFreeAlignResult(res);
                ed_ress[pair_idx] = res;
        }
    };
    long long ns = measure_ns(workload);
    double aligns_per_second = 1.0*num_pairs * 1000*1000*1000 / ns;
    cout << fixed << setprecision(2);
    cout << "edlib: " << aligns_per_second << " aligns/second" << endl;

    vector<Alignment_t> alignments;
    for(pair_idx = 0; pair_idx < num_pairs; pair_idx++){
        Alignment_t al = edlib_to_alignment(ed_ress[pair_idx]);
        alignments.push_back(al);
        edlibFreeAlignResult(ed_ress[pair_idx]);
    }
    return alignments;
}

void benchmark_wfa_lm(Genome_t &reference, vector<Read_t> &reads, int threads){
    if(enable_log) cerr << "benchmark_wfa_lm()..." << endl;

    for (auto & c: reference.content) c = toupper(c);
    for(Read_t &read: reads)
        for (auto & c: read.content) c = toupper(c);

    if(enable_log) cerr << "building input pairs..." << endl;
    size_t num_pairs = 0;
    for(Read_t &read : reads){
        num_pairs += read.locations.size();
    }

    struct wfalm_input_pair{
        size_t pattern_i;
        size_t text_start;
        size_t text_len;
    };
    vector<wfalm_input_pair> inputs;

    size_t pair_idx = 0;
    for(size_t i = 0; i < reads.size(); i++){
        for(CandidateLocation_t &location : reads[i].locations){
            long long sir = location.start_in_reference;
            wfalm_input_pair input;
            input.pattern_i = i;
            input.text_start = sir;
            input.text_len = min((unsigned long long)reads[i].content.size(), reference.content.size() - sir);
            inputs.push_back(input);
            pair_idx++;
        }
    }

    if(enable_log) cerr << "starting algorithm..." << endl;
    auto workload = [&](){
        omp_set_num_threads(threads);
        #pragma omp parallel for schedule(dynamic)
        for(long long pair_idx = 0; pair_idx < num_pairs; pair_idx++){
            wfalm::SWGScores scores;
            scores.match = 1;
            scores.gap_extend = 1;
            scores.gap_open = 1;
            scores.mismatch = 1;
            string pattern = reads[inputs[pair_idx].pattern_i].content;
            string text = reference.content.substr(inputs[pair_idx].text_start, inputs[pair_idx].text_len);
            auto aln_res = wfalm::wavefront_align_low_mem(text, pattern, scores);
        }
    };
    long long ns = measure_ns(workload);
    double aligns_per_second = 1.0*num_pairs * 1000*1000*1000 / ns;
    cout << fixed << setprecision(2);
    cout << "wfa_lm: " << aligns_per_second << " aligns/second" << endl;    
}

vector<Alignment_t> benchmark_genasm_cpu(Genome_t &reference, vector<Read_t> &reads, int threads){
    if(enable_log) cerr << "benchmark_genasm_cpu()..." << endl;

    genasm_cpu::enabled_algorithm_log = enable_log;
    long long core_algorithm_ns;
    vector<Alignment_t> alignments = genasm_cpu::align_all(reference, reads, threads, &core_algorithm_ns);

    double aligns_per_second = 1.0*alignments.size() * 1000*1000*1000 / core_algorithm_ns;
    cout << fixed << setprecision(2);
    cout << "genasm_cpu: " << aligns_per_second << " aligns/second" << endl;

    return alignments;
}

vector<Alignment_t> benchmark_custom_gact(Genome_t &reference, vector<Read_t> &reads, AffineGapCosts agc){
    for (auto & c: reference.content) c = toupper(c);
    for(Read_t &read: reads)
        for (auto & c: read.content) c = toupper(c);

    vector<Alignment_t> alignments;

    size_t pair_idx = 0;
    for(size_t i = 0; i < reads.size(); i++){
        for(CandidateLocation_t &location : reads[i].locations){
            string query = reads[i].content;
            string target = reference.content.substr(location.start_in_reference, reads[i].content.size());

            int T = 320;
            int O = 120;
            string cigar = custom_gact(query, target, -agc.gap_open_cost, -agc.gap_extend_cost, agc.match_bonus, -agc.mismatch_cost, T, O);
            
            Alignment_t aln;
            aln.cigar = cigar;
            alignments.push_back(aln);

            pair_idx++;
        }
    }

    cout << fixed << setprecision(2);
    cout << "custom_gact: " << 1.0 << " aligns/second" << endl;

    return alignments;
}

#ifdef LIB_WFA
void benchmark_wfa_exact(Genome_t &reference, vector<Read_t> reads, int threads){
    if(enable_log) cerr << "benchmark_wfa_exact()..." << endl;
    long long n = reference.content.size();

    for (auto & c: reference.content) c = toupper(c);
    for(Read_t &read: reads)
        for (auto & c: read.content) c = toupper(c);

    if(enable_log) cerr << "generating input pairs..." << endl;
    affine_penalties_t affine_penalties = {
        .match = 0,
        .mismatch = 1,
        .gap_opening = 1,
        .gap_extension = 1
    };

    struct wfa_input_pair {
        char *text;
        char *pattern;
        long long n;
        long long m;
    };
    vector<wfa_input_pair> inputs;
    for(Read_t &read : reads){
        for(CandidateLocation_t &location : read.locations){
            wfa_input_pair input;
            input.pattern = (char*)read.content.c_str();
            input.m = read.content.size();
            input.text = (char*)reference.content.c_str() + location.start_in_reference;
            input.n = min((long long)(input.m), n - location.start_in_reference);
            inputs.push_back(input);
        }
    }

    size_t max_read_length = 0;
    size_t num_pairs = 0;
    for(Read_t &read : reads){
        max_read_length = max(max_read_length, read.content.size());
        num_pairs += read.locations.size();
    }

    if(enable_log) cerr << "starting wfa_exact..." << endl;
    auto workload = [&](){
        omp_set_num_threads(threads);
        #pragma omp parallel
        {
            mm_allocator_t* mm_allocator = mm_allocator_new(BUFFER_SIZE_8M);
            affine_wavefronts_t* affine_wavefronts = affine_wavefronts_new_complete(max_read_length, max_read_length*105/100, &affine_penalties, NULL, mm_allocator);

            #pragma omp for
            for(long long pair_idx = 0; pair_idx < num_pairs; pair_idx++){
                affine_wavefronts_align(affine_wavefronts, inputs[pair_idx].pattern, inputs[pair_idx].m, inputs[pair_idx].text, inputs[pair_idx].n);
                affine_wavefronts_clear(affine_wavefronts);
            }

            affine_wavefronts_delete(affine_wavefronts);
            mm_allocator_delete(mm_allocator);
        }
    };
    long long ns = measure_ns(workload);
    double aligns_per_second = 1.0*num_pairs * 1000*1000*1000 / ns;
    cout << fixed << setprecision(2);
    cout << "wfa_exact: " << aligns_per_second << " aligns/second" << endl;
}

void benchmark_wfa_adaptive(Genome_t &reference, vector<Read_t> reads, int threads){
    if(enable_log) cerr << "benchmark_wfa_adaptive()..." << endl;
    long long n = reference.content.size();

    for (auto & c: reference.content) c = toupper(c);
    for(Read_t &read: reads)
        for (auto & c: read.content) c = toupper(c);

    affine_penalties_t affine_penalties = {
        .match = 0,
        .mismatch = 1,
        .gap_opening = 1,
        .gap_extension = 1
    };
    const int min_wavefront_length = 10;
    const int max_distance_threshold = 50;

    struct wfa_input_pair {
        char *text;
        char *pattern;
        long long n;
        long long m;
    };
    vector<wfa_input_pair> inputs;
    for(Read_t &read : reads){
        for(CandidateLocation_t &location : read.locations){
            wfa_input_pair input;
            input.pattern = (char*)read.content.c_str();
            input.m = read.content.size();
            input.text = (char*)reference.content.c_str() + location.start_in_reference;
            input.n = min((long long)(input.m), n - location.start_in_reference);
            inputs.push_back(input);
        }
    }

    size_t max_read_length = 0;
    size_t num_pairs = 0;
    for(Read_t &read : reads){
        max_read_length = max(max_read_length, read.content.size());
        num_pairs += read.locations.size();
    }

    auto workload = [&](){
        omp_set_num_threads(threads);
        #pragma omp parallel
        {
            mm_allocator_t* mm_allocator = mm_allocator_new(BUFFER_SIZE_8M);
            affine_wavefronts_t* affine_wavefronts = affine_wavefronts_new_reduced(max_read_length, max_read_length*105/100, &affine_penalties, min_wavefront_length, max_distance_threshold, NULL, mm_allocator);

            #pragma omp for
            for(long long pair_idx = 0; pair_idx < num_pairs; pair_idx++){
                affine_wavefronts_align(affine_wavefronts, inputs[pair_idx].pattern, inputs[pair_idx].m, inputs[pair_idx].text, inputs[pair_idx].n);
                affine_wavefronts_clear(affine_wavefronts);
            }

            affine_wavefronts_delete(affine_wavefronts);
            mm_allocator_delete(mm_allocator);
        }
    };
    long long ns = measure_ns(workload);
    double aligns_per_second = 1.0*num_pairs * 1000*1000*1000 / ns;
    cout << fixed << setprecision(2);
    cout << "wfa_adaptive: " << aligns_per_second << " aligns/second" << endl;
}

vector<Alignment_t> benchmark_wfa_adaptive_accuracy(Genome_t &reference, vector<Read_t> reads, AffineGapCosts agc){
    if(enable_log) cerr << "benchmark_wfa_adaptive()..." << endl;
    long long n = reference.content.size();

    for (auto & c: reference.content) c = toupper(c);
    for(Read_t &read: reads)
        for (auto & c: read.content) c = toupper(c);

    affine_penalties_t affine_penalties = {
        .match = 0,
        .mismatch = agc.mismatch_cost,
        .gap_opening = agc.gap_open_cost,
        .gap_extension = agc.gap_extend_cost
    };
    const int min_wavefront_length = 10;
    const int max_distance_threshold = 50;

    struct wfa_input_pair {
        char *text;
        char *pattern;
        long long n;
        long long m;
    };
    vector<wfa_input_pair> inputs;
    for(Read_t &read : reads){
        for(CandidateLocation_t &location : read.locations){
            wfa_input_pair input;
            input.pattern = (char*)read.content.c_str();
            input.m = read.content.size();
            input.text = (char*)reference.content.c_str() + location.start_in_reference;
            input.n = min((long long)(input.m), n - location.start_in_reference);
            inputs.push_back(input);
        }
    }

    size_t max_read_length = 0;
    size_t num_pairs = 0;
    for(Read_t &read : reads){
        max_read_length = max(max_read_length, read.content.size());
        num_pairs += read.locations.size();
    }

    mm_allocator_t* mm_allocator = mm_allocator_new(BUFFER_SIZE_8M);
    affine_wavefronts_t* affine_wavefronts = affine_wavefronts_new_reduced(max_read_length, max_read_length*105/100, &affine_penalties, min_wavefront_length, max_distance_threshold, NULL, mm_allocator);

    vector<Alignment_t> alignments;
    for(long long pair_idx = 0; pair_idx < num_pairs; pair_idx++){
        affine_wavefronts_align(affine_wavefronts, inputs[pair_idx].pattern, inputs[pair_idx].m, inputs[pair_idx].text, inputs[pair_idx].n);
        string cigar = "";

        edit_cigar_t* const edit_cigar = &affine_wavefronts->edit_cigar;
        for (int i=edit_cigar->begin_offset;i<edit_cigar->end_offset;++i) {
            switch (edit_cigar->operations[i]) {
            case 'M': cigar += "1="; break;
            case 'X': cigar += "1X"; break;
            case 'D': cigar += "1D"; break;
            case 'I': cigar += "1I"; break;
            }
        }

        Alignment_t aln;
        aln.cigar = cigar;
        alignments.push_back(aln);

        affine_wavefronts_clear(affine_wavefronts);
    }

    affine_wavefronts_delete(affine_wavefronts);
    mm_allocator_delete(mm_allocator);

    cout << fixed << setprecision(2);
    cout << "wfa_adaptive: " << 1.0 << " aligns/second" << endl;

    return alignments;
}
#endif

long long get_alignment_score(Alignment_t &alignment, AffineGapCosts agc){
    long long score = 0;

    stringstream cigar_ss(alignment.cigar);
    cigar_ss.peek(); //would set the eof bit for the empty string
    
    bool was_gap = false;
    while(!cigar_ss.eof()){
        unsigned int edit_count;
        char edit_type;
        cigar_ss >> edit_count;
        cigar_ss >> edit_type;
        cigar_ss.peek(); //set the eof bit

        if(edit_type == '='){
            score += edit_count*agc.match_bonus;
            was_gap = false;
        }
        if(edit_type == 'X'){
            score -= edit_count*agc.mismatch_cost;
            was_gap = false;
        }
        if(edit_type == 'I' || edit_type == 'D'){
            if(!was_gap)
                score -= agc.gap_open_cost;
            score -= edit_count*agc.gap_extend_cost;
            was_gap = true;
        }
    }

    return score;
}

void benchmark_baselines(string reference_file, string reads_file, string seeds_file, vector<int> threads, vector<string> algorithms, AffineGapCosts agc, int read_length_cap=-1, int dataset_inflation=1){
    if(enable_log) cerr << "Reading reference sequence..." << endl;
    Genome_t genome = read_genome(reference_file);

    if(enable_log) cerr << "Reading reads files (~30 seconds)..." << endl;
    vector<Read_t> reads;
    read_fastq_and_seed_locations(genome, reads_file, seeds_file, reads);

    if(enable_log) cerr << "Filtering reads..." << endl;
    //filter out any reverse complement reads
    for(Read_t &read : reads){
        read.locations.erase(remove_if(
                read.locations.begin(),
                read.locations.end(),
                [](CandidateLocation_t const &l){ return l.strand==false; }
            ),
            read.locations.end()
        );
    }

    if(enable_log) cerr << "Sorting reads..." << endl;
    //sort reads in descending length
    sort(reads.begin(), reads.end(), [](Read_t &a, Read_t &b){return a.content.size() > b.content.size();});

    if(read_length_cap >= 0){
        for(Read_t &r: reads){
            r.content = r.content.substr(0, read_length_cap);
        }
    }

    if(dataset_inflation > 1){
        int old_size = reads.size();
        reads.resize(dataset_inflation*old_size);
        for(int i = 1; i < dataset_inflation; i++){
            copy_n(reads.begin(), old_size, reads.begin()+i*old_size);
        }
    }

    //reads.erase(reads.begin()+1000, reads.end());
    if(enable_log) cout << reads.size() << " reads" << endl;

    #define ALG_IF(STR) if(find(algorithms.begin(), algorithms.end(), STR) != algorithms.end())
    for(int thread_count : threads){
        cout << thread_count << " threads" << endl;
        ALG_IF("edlib") benchmark_edlib(genome, reads, thread_count);
        ALG_IF("ksw2_extz") benchmark_ksw2_extz(genome, reads, agc, thread_count);
        ALG_IF("ksw2_extz2_sse") benchmark_ksw2_extz2_sse(genome, reads, agc, thread_count);
        ALG_IF("genasm_cpu") benchmark_genasm_cpu(genome, reads, thread_count);
        ALG_IF("wfa_lm") benchmark_wfa_lm(genome, reads, thread_count);
        #ifdef LIB_WFA
            ALG_IF("wfa_exact") benchmark_wfa_exact(genome, reads, thread_count);
            ALG_IF("wfa_adaptive") benchmark_wfa_adaptive(genome, reads, thread_count);
        #endif
        cout << endl;
    }
}

void accuracy_baselines(string reference_file, string reads_file, string seeds_file, vector<int> threads, vector<string> algorithms, bool print_cigar, AffineGapCosts agc, int read_length_cap=-1, int dataset_inflation=1){
    if(enable_log) cerr << "Reading reference sequence..." << endl;
    Genome_t genome = read_genome(reference_file);

    if(enable_log) cerr << "Reading reads files (~30 seconds)..." << endl;
    vector<Read_t> reads;
    read_fastq_and_seed_locations(genome, reads_file, seeds_file, reads);

    if(enable_log) cerr << "Filtering reads..." << endl;
    //filter out any reverse complement reads
    for(Read_t &read : reads){
        read.locations.erase(remove_if(
                read.locations.begin(),
                read.locations.end(),
                [](CandidateLocation_t const &l){ return l.strand==false; }
            ),
            read.locations.end()
        );
    }

    if(enable_log) cerr << "Sorting reads..." << endl;
    //sort reads in descending length
    sort(reads.begin(), reads.end(), [](Read_t &a, Read_t &b){return a.content.size() > b.content.size();});

    if(read_length_cap >= 0){
        for(Read_t &r: reads){
            r.content = r.content.substr(0, read_length_cap);
        }
    }

    if(dataset_inflation > 1){
        int old_size = reads.size();
        reads.resize(dataset_inflation*old_size);
        for(int i = 1; i < dataset_inflation; i++){
            copy_n(reads.begin(), old_size, reads.begin()+i*old_size);
        }
    }

    if(enable_log) cout << reads.size() << " reads" << endl;

    #define ALG_IF(STR) if(find(algorithms.begin(), algorithms.end(), STR) != algorithms.end())
    for(int thread_count : threads){
        cout << thread_count << " threads" << endl;
        for(string algorithm : algorithms){
            vector<Alignment_t> alignments;
            if(algorithm == "ksw2_extz"){
                alignments = benchmark_ksw2_extz(genome, reads, agc, thread_count);
            }
            if(algorithm == "ksw2_extz2_sse"){
                alignments = benchmark_ksw2_extz2_sse(genome, reads, agc, thread_count);
            }
            if(algorithm == "edlib"){
                alignments = benchmark_edlib(genome, reads, thread_count);
            }
            if(algorithm == "genasm_cpu"){
                alignments = benchmark_genasm_cpu(genome, reads, thread_count);
            }
            if(algorithm == "custom_gact"){
                alignments = benchmark_custom_gact(genome, reads, agc);
            }
            #ifdef LIB_WFA
            if(algorithm == "wfa_adaptive"){
                alignments = benchmark_wfa_adaptive_accuracy(genome, reads, agc);
            }
            #endif

            size_t pair_idx = 0;
            for(Read_t &read : reads){
                for(CandidateLocation_t &location : read.locations){
                    Alignment_t &al = alignments[pair_idx];
                    long long score = get_alignment_score(al, agc);
                    cout << "pair_idx=" << pair_idx << " score=" << score;
                    if(print_cigar){
                        cout << " cigar=" << al.cigar;
                        cout << " read=" << read.content;
                        cout << " reference=" << genome.content.substr(location.start_in_reference, read.content.size());
                    }
                    cout << endl;

                    pair_idx++;
                }
            }
        }
        //ALG_IF("genasm_cpu") benchmark_genasm_cpu(genome, reads, thread_count);
        //ALG_IF("wfa_lm") benchmark_wfa_lm(genome, reads, thread_count);
        //#ifdef LIB_WFA
        //    ALG_IF("wfa_exact") benchmark_wfa_exact(genome, reads, thread_count);
        //    ALG_IF("wfa_adaptive") benchmark_wfa_adaptive(genome, reads, thread_count);
        //#endif
        cout << endl;
    }
}

void parse_args(int argc, char **argv, string &reference_file, string &reads_file, string &seeds_file, vector<int> &threads, AffineGapCosts &agc, bool &verbose, bool &accuracy, bool &print_cigar, vector<string> &algorithms){
    //default values
    reference_file = "datasets/human_genome/pacbio-chr1-simulated-m10k-k5_0001.ref";
    reads_file =     "datasets/human_genome/pacbio-chr1-simulated-m10k-k5_0001.fastq";
    seeds_file =     "datasets/human_genome/pacbio-chr1-simulated-m10k-k5_0001.maf";
    string threads_str =  "24";
    string algorithms_str = "all";
    string scoring_str = "2,4,4,2";

    bool help_and_exit = false;
    help_and_exit |= OPT_INVALID == get_cmd_option(argc, argv, "--reference", reference_file);
    help_and_exit |= OPT_INVALID == get_cmd_option(argc, argv, "--reads", reads_file);
    help_and_exit |= OPT_INVALID == get_cmd_option(argc, argv, "--seeds", seeds_file);
    help_and_exit |= OPT_INVALID == get_cmd_option(argc, argv, "--threads", threads_str);
    help_and_exit |= OPT_INVALID == get_cmd_option(argc, argv, "--algorithms", algorithms_str);
    help_and_exit |= OPT_INVALID == get_cmd_option(argc, argv, "--scoring", scoring_str);
    help_and_exit |= OPT_INVALID == get_cmd_option(argc, argv, "--verbose");
    help_and_exit |= OPT_INVALID == get_cmd_option(argc, argv, "--accuracy");
    help_and_exit |= OPT_INVALID == get_cmd_option(argc, argv, "--cigar");
    help_and_exit |= OPT_MISSING != get_cmd_option(argc, argv, "--help");
    help_and_exit |= !check_options(argc, argv, {"--reference", "--reads", "--seeds", "--help", "--threads", "--algorithms", "--verbose", "--accuracy", "--cigar", "--scoring"});

    verbose = OPT_EXISTS == get_cmd_option(argc, argv, "--verbose");
    accuracy = OPT_EXISTS == get_cmd_option(argc, argv, "--accuracy");
    print_cigar = OPT_EXISTS == get_cmd_option(argc, argv, "--cigar");
    threads = parse_csv_numbers(threads_str);
    algorithms = parse_csv_strings(algorithms_str);
    if(find(algorithms.begin(), algorithms.end(), "all") != algorithms.end()){
        algorithms = vector<string>{"edlib", "ksw2_extz", "ksw2_extz2_sse", "wfa_exact", "wfa_adaptive", "wfa_lm", "genasm_cpu"};
    }

    vector<int> scores = parse_csv_numbers(scoring_str);
    agc.match_bonus = scores[0];
    agc.mismatch_cost = scores[1];
    agc.gap_open_cost = scores[2];
    agc.gap_extend_cost = scores[3];

    string help_text =
        "cpu_baseline[.exe] [options]\n"
        "Options:\n"
        "--reference=[path to reference FASTA] -- overide default reference data for performance test\n"
        "--reads=[path to reads FASTQ]         -- overide default reads data for performance test\n"
        "--seeds=[path to MAF or PAF]          -- overide default seeds data for performance test\n"
        "--threads=[THREADS[,MORE_THREADS]]    -- run benchmarks with the given list of thread counts default:24\n"
        "--algorithms=[ALGORITHM[,MORE ALGORITHMS]]] -- run only the specified baseline algorithms, supported are: edlib, ksw2_extz, ksw2_extz2_sse, wfa_exact, wfa_adaptive, wfa_lm, genasm_cpu, gact_custom\n"
        "--scoring=[MAT],[SUB],[GAPO],[GAPE]   -- set affine gap model scoring function, all values should be positive default:2,4,4,2\n"
        "--verbose                             -- print progress to stderr. Otherwise, only benchmark result are printed\n"
        "--accuracy                            -- print alignment score for each pair (do not run performance experiments)\n"
        "--cigar                               -- print cigar string for each pair (requires --accuracy)\n"
        "--help                                -- displays this information\n";

    if(help_and_exit){
        cout << help_text << flush;
        exit(0);
    }
}

int main(int argc, char **argv){
    string reference_file, reads_file, seeds_file;
    vector<int> threads;
    vector<string> algorithms;
    AffineGapCosts agc;
    bool verbose, accuracy, print_cigar;

    parse_args(argc, argv, reference_file, reads_file, seeds_file, threads, agc, verbose, accuracy, print_cigar, algorithms);
    enable_log = verbose;
    genasm_cpu::enabled_algorithm_log = verbose;

    if(accuracy){
        accuracy_baselines(reference_file, reads_file, seeds_file, threads, algorithms, print_cigar, agc);
    }
    else{
        benchmark_baselines(reference_file, reads_file, seeds_file, threads, algorithms, agc);
    }

    return 0;
}
