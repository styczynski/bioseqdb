#GPU compute capability
ARCH = $(GPU_ARCH)
CXX = $(nvccinstance)
NVCC = $(nvccinstance)

#check 32-bit or 64-bit architecture
HOST_ARCH = $(shell uname -m | grep 64)
LIB_ARCH =
ifneq "$(HOST_ARCH)" ""
	LIB_ARCH = _x86_64
endif

#have the support for Intel SSSE3?
have_ssse3 = 0
ifneq ($(have_ssse3), 0)
  MACROS += -DHAVE_SSSE3
  SSE_ARCH = -msse4
else
  MACROS +=
  SSE_ARCH = -msse2
endif

#other macros
#runtime information for CPU and GPU SIMD computation
MACROS += -DRT_DEBUG

#diable CPU threads
MACROS += -DDISABLE_CPU_THREADS

#define the maximal sequence lengths on GPUs
MACROS += -DMAX_SEQ_LENGTH_THRESHOLD=$(sequence_length_limit)

#compiling parameters
NVCCOPTIONS = -arch $(ARCH) --ptxas-options=-v -use_fast_math -Xcompiler -funroll-loops -Xcompiler $(SSE_ARCH) $(MACROS) -Xcompiler -ansi
NVCCFLAGS = -O3 $(NVCCOPTIONS) -I.

#determine the proper library as per the architecture
NVCCLIBS = -lm

#source files
CXX_src = CFastaFile.cpp CParams.cpp Aligner.cpp
NVCC_src = GenericFunction.cu CFastaSW.cu CFastaSWScalar.cu CSearch.cu CSearchScalar.cu CSearchMGPUScalar.cu main.cu

src_dir = src
objs_dir = objs
objs = $(patsubst %.cpp, $(objs_dir)/%.cpp.o, $(CXX_src))
objs += $(patsubst %.cu, $(objs_dir)/%.cu.o, $(NVCC_src))

EXEC = cudasw

all: dir $(objs)
	$(NVCC) $(NVCCFLAGS) -o $(EXEC) $(objs) $(NVCCLIBS)
	strip $(EXEC)

dir:
	mkdir -p $(objs_dir)

$(objs_dir)/%.cpp.o: $(src_dir)/%.cpp
	$(NVCC) $(NVCCFLAGS) -c $< -o $@

$(objs_dir)/%.cu.o: $(src_dir)/%.cu
	$(NVCC) $(NVCCFLAGS) -c $< -o $@

clean: 
	rm -rf $(EXEC) $(objs_dir) 

#dependence
CSearchScalar.cu: CSearchScalar.h

