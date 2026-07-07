MAX_QUERY_LEN=10000
N_CODE=0x4E
N_PENALTY=1

# Directories
SRC_DIR := src
INCLUDE_DIR := include
OBJ_DIR := obj
BIN_DIR := bin

# Files
SRC := $(wildcard $(SRC_DIR)/*.cpp)
CU_SRC := $(wildcard $(SRC_DIR)/*.cu)
INCLUDES := -I$(INCLUDE_DIR)
OBJ := $(patsubst $(SRC_DIR)/%.cpp, $(OBJ_DIR)/%.o, $(SRC))
CU_OBJ := $(patsubst $(SRC_DIR)/%.cu, $(OBJ_DIR)/%.o, $(CU_SRC))
TARGET := $(BIN_DIR)/minimap2-gpu

# Compiler and flags
CXX := g++
NVCC := nvcc
CXXFLAGS := -std=c++11 -DMAX_QUERY_LEN=$(MAX_QUERY_LEN) -DN_CODE=$(N_CODE) $(INCLUDES) 
NVCCFLAGS := -arch=sm_86 -DMAX_QUERY_LEN=$(MAX_QUERY_LEN) -DN_CODE=$(N_CODE) $(INCLUDES)

ifeq ($(DEBUG),1)
  CXXFLAGS  += -DENABLE_DEBUG
  NVCCFLAGS += -DENABLE_DEBUG
endif

$(info CXXFLAGS=$(CXXFLAGS))
$(info NVCCFLAGS=$(NVCCFLAGS))


# Build rules
all: $(TARGET)

debug:
	$(MAKE) DEBUG=1

$(TARGET): $(OBJ) $(CU_OBJ)
	@mkdir -p $(BIN_DIR)
	$(NVCC) $(NVCCFLAGS) $^ -o $@ -lz -lcudart

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@ -lz

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cu
	@mkdir -p $(OBJ_DIR)
	$(NVCC) $(NVCCFLAGS) $(INCLUDES) -c $< -o $@ -lz 

clean:
	$(RM) -r $(OBJ_DIR) $(BIN_DIR)

.PHONY: all clean
