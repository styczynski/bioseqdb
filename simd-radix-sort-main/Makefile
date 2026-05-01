BUILD_DIR ?= ./build

CPPFLAGS = $(CXXFLAGS) -MMD -MP -std=c++20 -O3\
 -mavx512f -mavx512bw -mavx512dq -mavx512vl -mavx512vbmi -mavx512vbmi2\
 -Wall -Wextra -Wpedantic\
 -Werror

CXX ?= g++

BINARIES = $(patsubst src/%.cpp,%,$(wildcard src/*.cpp))

.PHONY: all clean $(BINARIES)
all: $(BINARIES)
$(BINARIES): %: $(BUILD_DIR)/%
clean:
	$(RM) -r $(BUILD_DIR)

ifeq ($(origin IPPROOT), undefined)
IPPFLAGS =
else
IPPFLAGS = -I$(IPPROOT)/include -DIPP_RADIX_IS_PRESENT_ -L$(IPPROOT)/lib/intel64 -lippcore -lipps
endif

$(addprefix $(BUILD_DIR)/,$(BINARIES)): $(BUILD_DIR)/%: src/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(IPPFLAGS) $< -o $@

-include $(BUILD_DIR)/*.d
