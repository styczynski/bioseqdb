##/*************************************************************************************
##                           The MIT License
##
##   BWA-MEM2  (Sequence alignment using Burrows-Wheeler Transform),
##   Copyright (C) 2019  Intel Corporation, Heng Li.
##
##   Permission is hereby granted, free of charge, to any person obtaining
##   a copy of this software and associated documentation files (the
##   "Software"), to deal in the Software without restriction, including
##   without limitation the rights to use, copy, modify, merge, publish,
##   distribute, sublicense, and/or sell copies of the Software, and to
##   permit persons to whom the Software is furnished to do so, subject to
##   the following conditions:
##
##   The above copyright notice and this permission notice shall be
##   included in all copies or substantial portions of the Software.
##
##   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
##   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
##   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
##   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
##   BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
##   ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
##   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
##   SOFTWARE.
##
##Contacts: Vasimuddin Md <vasimuddin.md@intel.com>; Sanchit Misra <sanchit.misra@intel.com>;
##                                Heng Li <hli@jimmy.harvard.edu> 
##*****************************************************************************************/


EXE=		fmi_seeding_big_read ert_seeding_big_read  learned_seeding_big_read star_align
CXX=		g++
CXXFLAGS=	-std=c++17 -g -fpermissive -O3  -fopenmp -mtune=native -mavx512bw -DMODE=3

CPPFLAGS=	-DENABLE_PREFETCH  -DSAIS=1
INCLUDES=	-I../src  -I../ext/safestringlib/include  
LIBS=	    -L..  -fopenmp -lz -lbwa -L../ext/safestringlib/libsafestring.a  -L../ext/safestringlib -lsafestring 

#CPPFLAGS=	-DENABLE_PREFETCH -DVTUNE_ANALYSIS -DSAIS=1
#INCLUDES=	-I../src  -I../ext/safestringlib/include  -I/opt/intel/oneapi/vtune/2021.6.0/include
#LIBS=	    -L..  -fopenmp -lz -lbwa -L../ext/safestringlib/libsafestring.a  -L../ext/safestringlib -lsafestring -L/opt/intel/oneapi/vtune/2021.6.0/sdk/lib64 -littnotify -ldl
OBJS=		../src/fastmap.o ../src/main.o ../src/utils.o ../src/memcpy_bwamem.o ../src/kthread.o \
			../src/kstring.o ../src/ksw.o ../src/bwt.o ../src/ertindex.o ../src/Learnedindex.o ../src/bntseq.o ../src/bwamem.o ../src/ertseeding.o ../src/LearnedIndex_seeding.o ../src/profiling.o ../src/bandedSWA.o \
			../src/FMI_search.o ../src/read_index_ele.o ../src/bwamem_pair.o ../src/kswv.o ../src/bwa.o \
			../src/bwamem_extra.o ../src/bwtbuild.o ../src/QSufSort.o ../src/bwt_gen.o ../src/rope.o ../src/rle.o ../src/is.o ../src/kopen.o ../src/bwtindex.o
.PHONY:all clean depend
.SUFFIXES:.cpp .o

.cpp.o:
	$(CXX) -c $(CXXFLAGS) $(CPPFLAGS) $(INCLUDES) $< -o $@

all:$(EXE)

libbwa.a:$(OBJS)
	ar rcs libbwa.a $(OBJS)

fmi_seeding: FMI_seeding_reference.o 
	$(CXX)  $(CXXFLAGS) $(CPPFLAGS) -o $@ $^ $(LIBS)

fmi_seeding_big_read: FMI_seeding_reference_big_read.o  
	$(CXX)  $(CXXFLAGS) $(CPPFLAGS) -o $@ $^ $(LIBS)

learned_test: Learned_index_seeding_test.o libbwa.a
	$(CXX)  $(CXXFLAGS) $(CPPFLAGS) -o $@  $^ $(LIBS) 

learned_seeding_big_read: Learned_seeding_big_read.o libbwa.a
	$(CXX)  $(CXXFLAGS) $(CPPFLAGS) -o $@  $^ $(LIBS) 

meme_align: MEME_aligner.o libbwa.a
	$(CXX)  $(CXXFLAGS) $(CPPFLAGS) -o $@  $^ $(LIBS) 

star_align: Star_aligner.o libbwa.a
	$(CXX)  $(CXXFLAGS) $(CPPFLAGS) -o $@  $^ $(LIBS) 

fmi_align: FMI_aligner.o  
	$(CXX)  $(CXXFLAGS) $(CPPFLAGS) -o $@ $^ $(LIBS)

loading_test: Learned_index_loadtime_test.o libbwa.a
	$(CXX)  $(CXXFLAGS) $(CPPFLAGS) -o $@ $^ $(LIBS)

ert_test: ERT_seeding_test.o libbwa.a
	$(CXX)  $(CXXFLAGS) $(CPPFLAGS) -o $@  $^ $(LIBS) 

ert_seeding_big_read: ERT_seeding_big_read.o libbwa.a
	$(CXX)  $(CXXFLAGS) $(CPPFLAGS) -o $@  $^ $(LIBS) 

fmi_test:fmi_test.o
	$(CXX) -c $(CXXFLAGS) $(CPPFLAGS) -o $@ $^ $(LIBS)

smem2_test: smem2_test.o
	$(CXX) -o $@ $^ $(LIBS)

sa2ref_test: sa2ref_test.o
	$(CXX) -o $@ $^ $(LIBS)

bwt_seed_strategy_test:bwt_seed_strategy_test.o
	$(CXX) -o $@ $^ $(LIBS)

xeonbsw:main_banded.o
	$(CXX) -o $@ $^ $(LIBS)

clean:
	rm -fr *.o $(EXE)


# DO NOT DELETE
FMI_seeding_reference.o: ../src/FMI_search.h ../src/bntseq.h ../src/read_index_ele.h 
FMI_seeding_reference.o: ../src/bwa.h ../src/bwt.h ../src/utils.h ../src/macro.h  

FMI_seeding_reference_big_read.o: ../src/FMI_search.h ../src/bntseq.h ../src/read_index_ele.h  ../src/kstring.h ../src/ksw.h ../src/ertseeding.h ../src/bwamem.h
FMI_seeding_reference_big_read.o: ../src/bwa.h ../src/bwt.h ../src/kthread.h ../src/utils.h ../src/macro.h ../src/kseq.h  ../src/kvec.h
FMI_seeding_reference_big_read.o: ../src/ksort.h ../src/profiling.h  

FMI_aligner.o: ../src/FMI_search.h ../src/bntseq.h ../src/read_index_ele.h  ../src/kstring.h ../src/ksw.h ../src/ertseeding.h ../src/bwamem.h
FMI_aligner.o: ../src/bwa.h ../src/bwt.h ../src/kthread.h ../src/utils.h ../src/macro.h ../src/kseq.h  ../src/kvec.h
FMI_aligner.o: ../src/ksort.h ../src/profiling.h 


Learned_index_seeding_test.o: ../src/FMI_search.h ../src/bntseq.h ../src/read_index_ele.h  ../src/LearnedIndex_seeding.h ../src/Learnedindex.h ../src/bwamem.h ../src/kvec.h
Learned_index_seeding_test.o: ../src/bwa.h ../src/bwt.h ../src/utils.h ../src/macro.h 

Learned_seeding_big_read.o: ../src/FMI_search.h ../src/bntseq.h ../src/read_index_ele.h  ../src/LearnedIndex_seeding.h ../src/Learnedindex.h ../src/bwamem.h ../src/kvec.h
Learned_seeding_big_read.o: ../src/bwa.h ../src/bwt.h ../src/utils.h ../src/macro.h 

MEME_aligner.o: ../src/FMI_search.h ../src/bntseq.h ../src/read_index_ele.h  ../src/LearnedIndex_seeding.h ../src/Learnedindex.h ../src/bwamem.h ../src/kvec.h
MEME_aligner.o: ../src/bwa.h ../src/bwt.h ../src/utils.h ../src/macro.h 

Star_aligner.o: ../src/FMI_search.h ../src/bntseq.h ../src/read_index_ele.h  ../src/LearnedIndex_seeding.h ../src/Learnedindex.h ../src/bwamem.h ../src/kvec.h
Star_aligner.o: ../src/bwa.h ../src/bwt.h ../src/utils.h ../src/macro.h 

ERT_seeding_test.o: ../src/FMI_search.h ../src/bntseq.h ../src/read_index_ele.h   ../src/ertseeding.h ../src/bwamem.h ../src/kvec.h
ERT_seeding_test.o: ../src/bwa.h ../src/bwt.h ../src/utils.h ../src/macro.h 

ERT_seeding_big_read.o: ../src/FMI_search.h ../src/bntseq.h ../src/read_index_ele.h   ../src/ertseeding.h ../src/bwamem.h ../src/kvec.h
ERT_seeding_big_read.o: ../src/bwa.h ../src/bwt.h ../src/utils.h ../src/macro.h 

bwt_seed_strategy_test.o: ../src/FMI_search.h ../src/bntseq.h ../src/read_index_ele.h
bwt_seed_strategy_test.o: ../src/bwa.h ../src/bwt.h ../src/utils.h ../src/macro.h
fmi_test.o: ../src/FMI_search.h ../src/bntseq.h ../src/read_index_ele.h
fmi_test.o: ../src/bwa.h ../src/bwt.h ../src/utils.h ../src/macro.h
main_banded.o: ../src/bandedSWA.h ../src/macro.h
sa2ref_test.o: ../src/FMI_search.h ../src/bntseq.h ../src/read_index_ele.h
sa2ref_test.o: ../src/bwa.h ../src/bwt.h ../src/utils.h ../src/macro.h
smem2_test.o: ../src/FMI_search.h ../src/bntseq.h ../src/read_index_ele.h
smem2_test.o: ../src/bwa.h ../src/bwt.h ../src/utils.h ../src/macro.h

../src/LearnedIndex_seeding.o: ../src/LearnedIndex_seeding.h  ../src/bwamem.h ../src/bwt.h ../src/bntseq.h ../src/bwa.h ../src/macro.h
../src/LearnedIndex_seeding.o: ../src/kthread.h ../src/bandedSWA.h ../src/kstring.h ../src/ksw.h
../src/LearnedIndex_seeding.o: ../src/kvec.h ../src/ksort.h ../src/utils.h ../src/profiling.h
../src/LearnedIndex_seeding.o: ../src/FMI_search.h ../src/read_index_ele.h ../src/kbtree.h ../src/ertseeding.h




../src/FMI_search.o: ../src/FMI_search.h ../src/bntseq.h ../src/read_index_ele.h
../src/FMI_search.o: ../src/utils.h ../src/macro.h ../src/bwa.h ../src/bwt.h ../src/sais.h
../src/bandedSWA.o: ../src/bandedSWA.h ../src/macro.h
../src/bntseq.o: ../src/bntseq.h ../src/utils.h ../src/macro.h ../src/kseq.h ../src/khash.h
../src/bwa.o: ../src/bntseq.h ../src/bwa.h ../src/bwt.h ../src/macro.h ../src/ksw.h ../src/utils.h
../src/bwa.o: ../src/kstring.h ../src/kvec.h ../src/kseq.h
../src/bwamem.o: ../src/bwamem.h ../src/bwt.h ../src/bntseq.h ../src/bwa.h ../src/macro.h
../src/bwamem.o: ../src/kthread.h ../src/bandedSWA.h ../src/kstring.h ../src/ksw.h
../src/bwamem.o: ../src/kvec.h ../src/ksort.h ../src/utils.h ../src/profiling.h
../src/bwamem.o: ../src/FMI_search.h ../src/read_index_ele.h ../src/kbtree.h
../src/bwamem_extra.o: ../src/bwa.h ../src/bntseq.h ../src/bwt.h ../src/macro.h ../src/bwamem.h
../src/bwamem_extra.o: ../src/kthread.h ../src/bandedSWA.h ../src/kstring.h ../src/ksw.h
../src/bwamem_extra.o: ../src/kvec.h ../src/ksort.h ../src/utils.h ../src/profiling.h
../src/bwamem_extra.o: ../src/FMI_search.h ../src/read_index_ele.h
../src/bwamem_pair.o: ../src/kstring.h ../src/bwamem.h ../src/bwt.h ../src/bntseq.h
../src/bwamem_pair.o: ../src/bwa.h ../src/macro.h ../src/kthread.h ../src/bandedSWA.h
../src/bwamem_pair.o: ../src/ksw.h ../src/kvec.h ../src/ksort.h ../src/utils.h
../src/bwamem_pair.o: ../src/profiling.h ../src/FMI_search.h ../src/read_index_ele.h
../src/bwamem_pair.o: ../src/kswv.h
../src/bwt.o: ../src/utils.h ../src/bwt.h ../src/kvec.h ../src/malloc_wrap.h
../src/bwt_gen.o: ../src/QSufSort.h ../src/malloc_wrap.h
../src/bwtbuild.o: ../src/sais.h ../src/utils.h ../src/bntseq.h
../src/bwtindex.o: ../src/bntseq.h ../src/bwa.h ../src/bwt.h ../src/macro.h ../src/utils.h ../src/rle.h ../src/rope.h ../src/malloc_wrap.h
../src/bwtindex.o: ../src/bwtbuild.h
../src/bwtindex.o: ../src/FMI_search.h ../src/read_index_ele.h
../src/fastmap.o: ../src/fastmap.h ../src/bwa.h ../src/bntseq.h ../src/bwt.h ../src/macro.h
../src/fastmap.o: ../src/bwamem.h ../src/kthread.h ../src/bandedSWA.h ../src/kstring.h
../src/fastmap.o: ../src/ksw.h ../src/kvec.h ../src/ksort.h ../src/utils.h ../src/profiling.h
../src/fastmap.o: ../src/FMI_search.h ../src/read_index_ele.h ../src/kseq.h
../src/kstring.o: ../src/kstring.h
../src/ksw.o: ../src/ksw.h ../src/macro.h
../src/kswv.o: ../src/kswv.h ../src/macro.h ../src/ksw.h ../src/bandedSWA.h
../src/kthread.o: ../src/kthread.h ../src/macro.h ../src/bwamem.h ../src/bwt.h ../src/bntseq.h
../src/kthread.o: ../src/bwa.h ../src/bandedSWA.h ../src/kstring.h ../src/ksw.h ../src/kvec.h
../src/kthread.o: ../src/ksort.h ../src/utils.h ../src/profiling.h ../src/FMI_search.h
../src/kthread.o: ../src/read_index_ele.h
../src/main.o: ../src/main.h ../src/kstring.h ../src/utils.h ../src/macro.h ../src/bandedSWA.h
../src/main.o: ../src/profiling.h
../src/malloc_wrap.o: ../src/malloc_wrap.h
../src/profiling.o: ../src/macro.h
../src/read_index_ele.o: ../src/read_index_ele.h ../src/utils.h ../src/bntseq.h
../src/read_index_ele.o: ../src/macro.h
../src/utils.o: ../src/utils.h ../src/ksort.h ../src/kseq.h
../src/rle.o: ../src/rle.h
../src/rope.o: ../src/rle.h ../src/rope.h
../src/is.o: ../src/malloc_wrap.h
../src/QSufSort.o: ../src/QSufSort.h
../src/ertindex.o: ../src/ertindex.h ../src/bwt.h ../src/kvec.h ../src/macro.h
../src/ertseeding.o: ../src/ertseeding.h ../src/bwamem.h ../src/bwt.h ../src/bntseq.h ../src/bwa.h ../src/macro.h 
../src/ertseeding.o: ../src/kthread.h ../src/bandedSWA.h ../src/kstring.h ../src/ksw.h
../src/ertseeding.o: ../src/kvec.h ../src/ksort.h ../src/utils.h ../src/profiling.h
../src/ertseeding.o: ../src/FMI_search.h ../src/read_index_ele.h ../src/kbtree.h
../src/Learnedindex.o: ../src/Learnedindex.h ../src/bwt.h ../src/kvec.h ../src/macro.h
../src/LearnedIndex_seeding.o: ../src/LearnedIndex_seeding.h  ../src/bwamem.h ../src/bwt.h ../src/bntseq.h ../src/bwa.h ../src/macro.h
../src/LearnedIndex_seeding.o: ../src/kthread.h ../src/bandedSWA.h ../src/kstring.h ../src/ksw.h
../src/LearnedIndex_seeding.o: ../src/kvec.h ../src/ksort.h ../src/utils.h ../src/profiling.h
../src/LearnedIndex_seeding.o: ../src/FMI_search.h ../src/read_index_ele.h ../src/kbtree.h ../src/ertseeding.h
../src/memcpy_bwamem.o: ../src/memcpy_bwamem.h
