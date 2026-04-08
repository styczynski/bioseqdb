#if the env variable NVCC is set, use that nvcc instance, otherwise use the first nvcc in PATH
NVCC := $(if $(NVCC),$(NVCC),nvcc)
#if the env variable CXX is set, use that c++ compiler instance, otherwise use the first g++ in PATH
CXX := $(if $(CXX),$(CXX),g++)
#if the env variable NVCC_ARCH is set, call $NVCC with -arch=$NVCC_ARCH, otherwise use -arch=sm_86
NVCC_ARCH := $(if $(NVCC_ARCH),$(NVCC_ARCH),sm_86)

CXX_FLAGS := -O3 -std=c++17
NVCC_FLAGS := -arch=$(NVCC_ARCH) -lineinfo -rdc=true

SRC := src
SOURCES := genasm_gpu.cu genasm_cpu.cpp util.cpp bitvector_test.cu
SRCPATHS := $(SOURCES:%=$(SRC)/%)
HEADERS := bitvector_test.hpp bitvector.hpp cuda_list.hpp cuda_util.hpp filesystem.hpp genasm_cpu.hpp genasm_gpu.hpp util.hpp
HEADERPATHS := $(HEADERS:%=$(SRC)/%)

wfa_include := -DLIB_WFA -Ibaseline_algorithms/wfa -Lbaseline_algorithms/wfa/build
lib_wfa := -lwfa

tests_windows: $(SRC)/tests.cu $(SRCPATHS) $(HEADERPATHS)
	$(NVCC) $(NVCC_FLAGS) $(SRC)/tests.cu $(SRCPATHS) -o tests $(CXX_FLAGS) -Xcompiler "/openmp"

tests_linux: $(SRC)/tests.cu $(SRCPATHS) $(HEADERPATHS)
	$(NVCC) $(NVCC_FLAGS) $(SRC)/tests.cu $(SRCPATHS) -o tests $(CXX_FLAGS) -lstdc++fs -Xcompiler -fopenmp

cpu_baseline_windows: $(SRC)/cpu_baseline.cpp $(SRC)/genasm_cpu.cpp $(SRC)/util.cpp
	$(NVCC) -o cpu_baseline $(SRC)/cpu_baseline.cpp $(SRC)/genasm_cpu.cpp $(SRC)/util.cpp baseline_algorithms/ksw2/ksw2_extz.c baseline_algorithms/ksw2/ksw2_extz2_sse.c baseline_algorithms/edlib/edlib.cpp baseline_algorithms/gact/gact.cpp $(CXX_FLAGS) -D__SSE2__ -Xcompiler "/openmp"

cpu_baseline_linux: $(SRC)/cpu_baseline.cpp $(SRC)/genasm_cpu.cpp $(SRC)/util.cpp
	mkdir -p baseline_algorithms/wfa/build
	$(MAKE) -C baseline_algorithms/wfa/ lib_wfa
	$(CXX) -o cpu_baseline $(wfa_include) $(SRC)/cpu_baseline.cpp $(SRC)/genasm_cpu.cpp $(SRC)/util.cpp baseline_algorithms/ksw2/ksw2_extz.c baseline_algorithms/ksw2/ksw2_extz2_sse.c baseline_algorithms/edlib/edlib.cpp baseline_algorithms/gact/gact.cpp $(CXX_FLAGS) -lpthread -lstdc++fs $(lib_wfa) -fopenmp

library_example_windows: $(SRC)/library_example.cu $(SRCPATHS) $(HEADERPATHS)
	$(NVCC) $(NVCC_FLAGS) $(SRC)/library_example.cu $(SRCPATHS) -o library_example $(CXX_FLAGS) -Xcompiler "/openmp"

library_example_linux: $(SRC)/library_example.cu $(SRCPATHS) $(HEADERPATHS)
	$(NVCC) $(NVCC_FLAGS)  $(SRC)/library_example.cu $(SRCPATHS) -o library_example $(CXX_FLAGS) -lstdc++fs -Xcompiler -fopenmp
