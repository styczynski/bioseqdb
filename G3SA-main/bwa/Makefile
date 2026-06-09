# Compilers and flags
INCLUDES = -Isrc -Iext/zlib-1.3.1 -Iext/safestringlib/include -Iext/bwa-mem2/include
CXX = g++
CXXFLAGS = -Wall -std=c++11 -pthread $(INCLUDES) -MMD -MP -O3 #-lineinfo -g #-O0
CC = gcc
CFLAGS = -Wall -Wno-unused-function -O3 $(INCLUDES) -lm -MMD -MP
NVCC = nvcc
CU_ARCH = sm_86 # A6000: sm_86, rtx4090: sm_89
CU_COMPUTE_ARCH = $(subst sm,compute,$(CU_ARCH))

__NVFLAGS = -ccbin $(CXX) --gpu-architecture=$(CU_COMPUTE_ARCH) --gpu-code=$(CU_ARCH) --default-stream per-thread $(INCLUDES) -Xptxas -O4 -Xcompiler -O4
_NVFLAGS = -ccbin $(CXX) --gpu-architecture=$(CU_COMPUTE_ARCH) --gpu-code=$(CU_ARCH) --default-stream per-thread $(INCLUDES) -MMD -MP -dc
NVFLAGS = $(_NVFLAGS) -Xptxas -O4 -Xcompiler -O4  -lineinfo
NVFLAGS_DEBUG = $(_NVFLAGS) -G -g

LINKFLAGS = -Lext/zlib-1.3.1 -Lext/safestringlib -L/usr/local/cuda/lib64 -Lext/bwa-mem2 -pthread
LIBS = -lz -lcudart -lcudadevrt -lsafestring -lreadindexele -lbntseq

# Target executable
HASHER = hasher.exe
EXE = g3.exe

# Sources, objects and dependencies files
CPP_SOURCES = $(wildcard src/*.cpp)
C_SOURCES = $(wildcard src/*.c)
CU_SOURCES = $(wildcard src/*.cu)

CPP_OBJECTS = $(CPP_SOURCES:.cpp=.o)
CPP_OBJECTS_HASHER = src/buildIndex.o \
		      src/hashKMer.o \
		      src/loadKMerIndex.o
CPP_OBJECTS_EXE = src/loadKMerIndex.o \
		      src/timer.o \
		      src/fastmap.o \
		      src/main.o \
		      src/FMI_wrapper.o \
		      src/FMI_search.o \
		      src/utils.o \
		      src/kopen.o \
		      src/memcpy_bwamem.o \
		      src/pipeline.o

C_OBJECTS = $(C_SOURCES:.c=.o)

C_OBJECTS_EXE = src/malloc_wrap.o \
		    src/bwt.o \

CU_OBJECTS = $(CU_SOURCES:.cu=.o)

CU_OBJECTS_LINKER = src/gpu_link.o

# Compile rules
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<
%.o: %.cu
	$(NVCC) $(NVFLAGS) -c -o $@ $<

# Linking rule
$(HASHER): $(CPP_OBJECTS_HASHER)
	$(CXX) $(LINKFLAGS) -o $@ $^ -lz

$(EXE): $(CPP_OBJECTS_EXE) $(C_OBJECTS_EXE) $(CU_OBJECTS_LINKER) $(CU_OBJECTS) 
	$(CXX) $(LINKFLAGS) -L. -o $@ $^ $(LIBS) 

$(CU_OBJECTS_LINKER): $(CU_OBJECTS)
	$(NVCC) $(__NVFLAGS) -dlink $^ --output-file $@


# Make rules
all: $(EXE)

docs:
	cd assets/docs/ && ../../scripts/make-docs.sh # in docs

#tester
tester.o: tester_main.cu
	$(NVCC) $(NVFLAGS) -c -o $@ $^

dlink.o: src/gmem_alloc.o tester.o
	$(NVCC) $(__NVFLAGS) -dlink $^ --output-file $@

tester.exe: tester.o dlink.o
	nvcc -o tester.exe tester.o src/gmem_alloc.o dlink.o

clean:
	rm -f *.o *.d *.exe

cleanall:
	rm -f  $(CU_OBJECTS_LINKER)  $(CU_OBJECTS) $(CPP_OBJECTS) $(C_OBJECTS) $(HASHER) $(EXE)

.PHONY: all clean

###########################################################################
