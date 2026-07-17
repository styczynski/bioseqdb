################################################################################
################################################################################

# Location of the CUDA Toolkit
CUDA_PATH ?= /usr/local/cuda

# host compiler
CC = g++
NVCC := $(CUDA_PATH)/bin/nvcc -ccbin $(CC)

# CUDA library directory:
CUDA_LIB_DIR= -L$(CUDA_PATH)/lib64
# CUDA include directory:
CUDA_INC_DIR= -I$(CUDA_PATH)/include
# CUDA linking libraries:
CUDA_LINK_LIBS= -lcudart

NVCC_FLAGS :=
NVCC_LIBS :=
NVCC_LIBS += $(CUDA_LINK_LIBS)
NVCC_LIBS += $(CUDA_INC_DIR)
NVCC_LIBS += $(CUDA_LIB_DIR)

CCFLAGS := -O3 -Xcompiler -fopenmp
LDFLAGS += $(CCFLAGS)

# Common includes and paths for CUDA
SRC_DIR := src
INC_DIR := include
LIBRARIES :=
LIBRARIES += -lgomp -lm #omp support
LIBRARIES += $(NVCC_LIBS)

################################################################################
# MACROS

# Modify only these :
READ_LENGTH = 100
ERROR_THRESHOLD = 5
MAX_GPU = 8

#### DO NOT CHANGE BELOW THIS LINE---------------------------------------------
UINT_PARTITION_SIZE := 16

# WINDOW_COUNT = ceil(READ_LENGTH / UINT_PARTITION_SIZE)
WINDOW_COUNT := $(shell echo "scale=1; $(READ_LENGTH)/$(UINT_PARTITION_SIZE)" | bc)
DECIMAL := $(shell echo $(WINDOW_COUNT) | cut -f2 -d.)
CONDITION := $(shell [ $(DECIMAL) -gt 0 ] && echo true)

ifeq ($(CONDITION), true)
WINDOW_COUNT := $(shell echo $(WINDOW_COUNT)+1 | bc) 
endif
WINDOW_COUNT := $(shell echo $(WINDOW_COUNT) | cut -f1 -d.)

# A_WINDOW_COUNT = ceil(WINDOW_COUNT/2)
A_WINDOW_COUNT := $(shell echo "scale=1; $(WINDOW_COUNT)/2" | bc)
A_DECIMAL := $(shell echo $(A_WINDOW_COUNT) | cut -f2 -d.)
A_CONDITION := $(shell [ $(A_DECIMAL) -gt 0 ] && echo true)

ifeq ($(A_CONDITION), true)
A_WINDOW_COUNT := $(shell echo $(A_WINDOW_COUNT)+1 | bc) 
endif
A_WINDOW_COUNT := $(shell echo $(A_WINDOW_COUNT) | cut -f1 -d.)

TOTAL_MASKS = $(shell echo 2*${ERROR_THRESHOLD}+1)

MACRO_FLAGS := -DWINDOW_COUNT=$(WINDOW_COUNT)
MACRO_FLAGS += -DA_WINDOW_COUNT=$(A_WINDOW_COUNT)
MACRO_FLAGS += -DTOTAL_MASKS=$(TOTAL_MASKS)
MACRO_FLAGS += -DERROR_THRESHOLD=$(ERROR_THRESHOLD)
MACRO_FLAGS += -DMAX_GPU=$(MAX_GPU)
MACRO_FLAGS += -DREAD_LENGTH=$(READ_LENGTH)

################################################################################

all: build

build: gateKeeperGPU

OBJS = common.o cuda_config.o main.o kernel.o

common.o: $(SRC_DIR)/common.c
	$(NVCC) $(CCFLAGS) $(GENCODE_FLAGS) -o $@ -c $<

kernel.o: $(SRC_DIR)/kernel.cu
	$(NVCC) $(MACRO_FLAGS) $(CCFLAGS) $(GENCODE_FLAGS) -o $@ -c -dc $<

cuda_config.o: $(SRC_DIR)/cuda_config.c
	$(NVCC) $(MACRO_FLAGS) $(CCFLAGS) $(GENCODE_FLAGS) -o $@ -c $<

main.o: $(SRC_DIR)/main.c
	$(NVCC) $(MACRO_FLAGS) $(CCFLAGS) $(GENCODE_FLAGS) -lm -o $@ -c $<

gateKeeperGPU:$(OBJS)
	$(NVCC) $(LDFLAGS) $(GENCODE_FLAGS) -o $@ $+ $(LIBRARIES)

clean:
	rm -f gateKeeperGPU *.o

clobber: clean
