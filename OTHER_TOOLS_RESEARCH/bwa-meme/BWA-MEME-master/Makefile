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

ifneq ($(portable),)
	STATIC_GCC=-static-libgcc -static-libstdc++
endif

EXE=		bwa-meme
#CXX=		icpc

USE_MIMALLOC=1

# BWA-MEME Mode
# 1: Without 64bit key and ISA, 38GB for index
# 2: Without ISA, 88GB for index
# 3: BWA-MEME full, 118GB for index
ifeq ($(MODE),)
	MODE=3
endif

ifeq ($(CXX), icpc)
	CC= icc
else ifeq ($(CXX), g++)
	CC=gcc
endif		
ARCH_FLAGS=	-msse -msse2 -msse3 -mssse3 -msse4.1
MEM_FLAGS=	-DSAIS=1
CPPFLAGS+=	-DENABLE_PREFETCH -DV17=1 -DMATE_SORT=1 $(MEM_FLAGS) -DMODE=$(MODE)

INCLUDES=   -Isrc -Iext/safestringlib/include 
LIBS=		-lpthread -lm -lz -lisal -L. -lbwa  -Lext/safestringlib -lsafestring $(STATIC_GCC)

ifeq ($(USE_MIMALLOC), 1)
	MIMALLOC_LIB = build/mimalloc/libmimalloc.a
	LIBS+= -Imimalloc/include -Wl,-whole-archive $(MIMALLOC_LIB) -Wl,-no-whole-archive -lrt
endif

OBJS=		src/fastmap.o src/main.o src/utils.o src/memcpy_bwamem.o src/kthread.o \
			src/kstring.o src/ksw.o src/bwt.o src/ertindex.o src/Learnedindex.o src/bntseq.o src/bwamem.o src/ertseeding.o src/LearnedIndex_seeding.o src/profiling.o src/bandedSWA.o \
			src/FMI_search.o src/read_index_ele.o src/bwamem_pair.o src/kswv.o src/bwa.o \
			src/bwamem_extra.o src/bwtbuild.o src/QSufSort.o src/bwt_gen.o src/rope.o src/rle.o src/is.o src/kopen.o src/bwtindex.o src/fast_reader.o
BWA_LIB=    libbwa.a
SAFE_STR_LIB=    ext/safestringlib/libsafestring.a

ifeq ($(arch),sse41)
	ifeq ($(CXX), icpc)
		ARCH_FLAGS=-msse4.1
	else
		ARCH_FLAGS=-msse -msse2 -msse3 -mssse3 -msse4.1
	endif
else ifeq ($(arch),sse42)
	ifeq ($(CXX), icpc)	
		ARCH_FLAGS=-msse4.2
	else
		ARCH_FLAGS=-msse -msse2 -msse3 -mssse3 -msse4.1 -msse4.2
	endif
else ifeq ($(arch),avx)
	ifeq ($(CXX), icpc)
		ARCH_FLAGS=-mavx ##-xAVX
	else	
		ARCH_FLAGS=-mavx
	endif
else ifeq ($(arch),avx2)
	ifeq ($(CXX), icpc)
		ARCH_FLAGS=-march=core-avx2 #-xCORE-AVX2
	else	
		ARCH_FLAGS=-mavx2
	endif
else ifeq ($(arch),avx512)
	ifeq ($(CXX), icpc)
		ARCH_FLAGS=-xCORE-AVX512
	else	
		ARCH_FLAGS=-mavx512bw
	endif
else ifeq ($(arch),native)
	ARCH_FLAGS=-march=native
else ifneq ($(arch),)
# To provide a different architecture flag like -march=core-avx2.
	ARCH_FLAGS=$(arch)
else
myall:multi
endif
#add openmp for multi thread learned index build
CXXFLAGS+=	-g -O3 -fpermissive -fopenmp $(ARCH_FLAGS) #-Wall ##-xSSE2

.PHONY:all clean depend multi
.SUFFIXES:.cpp .o

.cpp.o:
	$(CXX) -c $(CXXFLAGS) $(CPPFLAGS) $(INCLUDES) $< -o $@

all:$(EXE) 

multi:
	rm -f src/*.o $(BWA_LIB); cd ext/safestringlib/ && $(MAKE) clean;
	$(MAKE) arch=sse41    EXE=bwa-meme_mode1.sse41 MODE=1    CXX=$(CXX) all
	rm -f src/*.o $(BWA_LIB);
	$(MAKE) arch=sse41    EXE=bwa-meme_mode2.sse41 MODE=2    CXX=$(CXX) all
	rm -f src/*.o $(BWA_LIB);
	$(MAKE) arch=sse41    EXE=bwa-meme_mode3.sse41 MODE=3    CXX=$(CXX) all

	rm -f src/*.o $(BWA_LIB); cd ext/safestringlib/ && $(MAKE) clean;
	$(MAKE) arch=sse42    EXE=bwa-meme_mode1.sse42 MODE=1    CXX=$(CXX) all
	rm -f src/*.o $(BWA_LIB);
	$(MAKE) arch=sse42    EXE=bwa-meme_mode2.sse42 MODE=2    CXX=$(CXX) all
	rm -f src/*.o $(BWA_LIB);
	$(MAKE) arch=sse42    EXE=bwa-meme_mode3.sse42 MODE=3    CXX=$(CXX) all

	rm -f src/*.o $(BWA_LIB); cd ext/safestringlib/ && $(MAKE) clean;
	$(MAKE) arch=avx    EXE=bwa-meme_mode1.avx  MODE=1  CXX=$(CXX) all
	rm -f src/*.o $(BWA_LIB);
	$(MAKE) arch=avx    EXE=bwa-meme_mode2.avx  MODE=2  CXX=$(CXX) all
	rm -f src/*.o $(BWA_LIB);
	$(MAKE) arch=avx    EXE=bwa-meme_mode3.avx  MODE=3  CXX=$(CXX) all

	rm -f src/*.o $(BWA_LIB); cd ext/safestringlib/ && $(MAKE) clean;
	$(MAKE) arch=avx2   EXE=bwa-meme_mode1.avx2   MODE=1  CXX=$(CXX) all
	rm -f src/*.o $(BWA_LIB);
	$(MAKE) arch=avx2   EXE=bwa-meme_mode2.avx2   MODE=2  CXX=$(CXX) all
	rm -f src/*.o $(BWA_LIB);
	$(MAKE) arch=avx2   EXE=bwa-meme_mode3.avx2   MODE=3  CXX=$(CXX) all

	rm -f src/*.o $(BWA_LIB); cd ext/safestringlib/ && $(MAKE) clean;
	$(MAKE) arch=avx512 EXE=bwa-meme_mode1.avx512bw MODE=1 CXX=$(CXX) all
	rm -f src/*.o $(BWA_LIB);
	$(MAKE) arch=avx512 EXE=bwa-meme_mode2.avx512bw MODE=2 CXX=$(CXX) all
	rm -f src/*.o $(BWA_LIB);
	$(MAKE) arch=avx512 EXE=bwa-meme_mode3.avx512bw MODE=3 CXX=$(CXX) all

	$(CXX) -Wall -O3 src/runsimd.cpp -DMODE=3 -Iext/safestringlib/include -Lext/safestringlib/ -lsafestring $(STATIC_GCC) -o bwa-meme
	$(CXX) -Wall -O3 src/runsimd.cpp -DMODE=2 -Iext/safestringlib/include -Lext/safestringlib/ -lsafestring $(STATIC_GCC) -o bwa-meme_mode2
	$(CXX) -Wall -O3 src/runsimd.cpp -DMODE=1 -Iext/safestringlib/include -Lext/safestringlib/ -lsafestring $(STATIC_GCC) -o bwa-meme_mode1

$(EXE):$(BWA_LIB) $(SAFE_STR_LIB) $(MIMALLOC_LIB) src/main.o 
	$(CXX) $(CXXFLAGS)  src/main.o $(BWA_LIB) -DMODE=$(MODE) $(LIBS) -o $@

$(MIMALLOC_LIB):
	mkdir -p build/mimalloc
	cd build/mimalloc; cmake ../../mimalloc; cd ../..
	$(MAKE) -C build/mimalloc mimalloc-static


$(BWA_LIB):$(OBJS)
	ar rcs $(BWA_LIB) $(OBJS)

$(SAFE_STR_LIB):
	cd ext/safestringlib/ && $(MAKE) clean && $(MAKE) CC=$(CC) directories libsafestring.a

clean:
	rm -fr src/*.o $(BWA_LIB) $(EXE) $(EXE)_mode1 $(EXE)_mode2 bwa-meme*.sse41 bwa-meme*.sse42 bwa-meme*.avx bwa-meme*.avx2 bwa-meme*.avx512bw
	rm -r build
	cd ext/safestringlib/ && $(MAKE) clean

depend:
	(LC_ALL=C; export LC_ALL; makedepend -Y -- $(CXXFLAGS) $(CPPFLAGS) -I. -- src/*.cpp)

# DO NOT DELETE
src/FMI_search.o: src/FMI_search.h src/bntseq.h src/read_index_ele.h
src/FMI_search.o: src/utils.h src/macro.h src/bwa.h src/bwt.h src/sais.h
src/bandedSWA.o: src/bandedSWA.h src/macro.h
src/bntseq.o: src/bntseq.h src/utils.h src/macro.h src/kseq.h src/khash.h
src/bwa.o: src/bntseq.h src/bwa.h src/bwt.h src/macro.h src/ksw.h src/utils.h
src/bwa.o: src/kstring.h src/kvec.h src/kseq.h
src/bwamem.o: src/bwamem.h src/bwt.h src/bntseq.h src/bwa.h src/macro.h
src/bwamem.o: src/kthread.h src/bandedSWA.h src/kstring.h src/ksw.h
src/bwamem.o: src/kvec.h src/ksort.h src/utils.h src/profiling.h
src/bwamem.o: src/FMI_search.h src/read_index_ele.h src/kbtree.h
src/bwamem_extra.o: src/bwa.h src/bntseq.h src/bwt.h src/macro.h src/bwamem.h
src/bwamem_extra.o: src/kthread.h src/bandedSWA.h src/kstring.h src/ksw.h
src/bwamem_extra.o: src/kvec.h src/ksort.h src/utils.h src/profiling.h
src/bwamem_extra.o: src/FMI_search.h src/read_index_ele.h
src/bwamem_pair.o: src/kstring.h src/bwamem.h src/bwt.h src/bntseq.h
src/bwamem_pair.o: src/bwa.h src/macro.h src/kthread.h src/bandedSWA.h
src/bwamem_pair.o: src/ksw.h src/kvec.h src/ksort.h src/utils.h
src/bwamem_pair.o: src/profiling.h src/FMI_search.h src/read_index_ele.h
src/bwamem_pair.o: src/kswv.h
src/bwt.o: src/utils.h src/bwt.h src/kvec.h src/malloc_wrap.h
src/bwt_gen.o: src/QSufSort.h src/malloc_wrap.h
src/bwtbuild.o: src/sais.h src/utils.h src/bntseq.h
src/bwtindex.o: src/bntseq.h src/bwa.h src/bwt.h src/macro.h src/utils.h src/rle.h src/rope.h src/malloc_wrap.h
src/bwtindex.o: src/bwtbuild.h
src/bwtindex.o: src/FMI_search.h src/read_index_ele.h
src/fastmap.o: src/fastmap.h src/bwa.h src/bntseq.h src/bwt.h src/macro.h 
src/fastmap.o: src/bwamem.h src/kthread.h src/bandedSWA.h src/kstring.h
src/fastmap.o: src/ksw.h src/kvec.h src/ksort.h src/utils.h src/profiling.h
src/fastmap.o: src/FMI_search.h src/read_index_ele.h src/kseq.h
src/kstring.o: src/kstring.h
src/ksw.o: src/ksw.h src/macro.h
src/kswv.o: src/kswv.h src/macro.h src/ksw.h src/bandedSWA.h
src/kthread.o: src/kthread.h src/macro.h src/bwamem.h src/bwt.h src/bntseq.h
src/kthread.o: src/bwa.h src/bandedSWA.h src/kstring.h src/ksw.h src/kvec.h
src/kthread.o: src/ksort.h src/utils.h src/profiling.h src/FMI_search.h
src/kthread.o: src/read_index_ele.h
src/main.o: src/main.h src/kstring.h src/utils.h src/macro.h src/bandedSWA.h
src/main.o: src/profiling.h
src/malloc_wrap.o: src/malloc_wrap.h
src/profiling.o: src/macro.h
src/read_index_ele.o: src/read_index_ele.h src/utils.h src/bntseq.h
src/read_index_ele.o: src/macro.h
src/utils.o: src/utils.h src/ksort.h src/kseq.h
src/rle.o: src/rle.h
src/rope.o: src/rle.h src/rope.h
src/is.o: src/malloc_wrap.h
src/QSufSort.o: src/QSufSort.h
src/ertindex.o: src/ertindex.h src/bwt.h src/kvec.h src/macro.h
src/ertseeding.o: src/ertseeding.h src/bwamem.h src/bwt.h src/bntseq.h src/bwa.h src/macro.h 
src/ertseeding.o: src/kthread.h src/bandedSWA.h src/kstring.h src/ksw.h
src/ertseeding.o: src/kvec.h src/ksort.h src/utils.h src/profiling.h
src/ertseeding.o: src/FMI_search.h src/read_index_ele.h src/kbtree.h
src/Learnedindex.o: src/Learnedindex.h src/bwt.h src/kvec.h src/macro.h
src/LearnedIndex_seeding.o: src/LearnedIndex_seeding.h src/fastmap.h src/bwamem.h src/bwt.h src/bntseq.h src/bwa.h src/macro.h
src/LearnedIndex_seeding.o: src/kthread.h src/bandedSWA.h src/kstring.h src/ksw.h
src/LearnedIndex_seeding.o: src/kvec.h src/ksort.h src/utils.h src/profiling.h
src/LearnedIndex_seeding.o: src/FMI_search.h src/read_index_ele.h src/kbtree.h src/ertseeding.h
src/memcpy_bwamem.o: src/memcpy_bwamem.h
