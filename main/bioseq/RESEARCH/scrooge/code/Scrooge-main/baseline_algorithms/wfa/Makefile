###############################################################################
# Flags & Folders
###############################################################################
FOLDER_BIN=bin
FOLDER_BUILD=build

UNAME=$(shell uname)

CC=gcc
CPP=g++

LD_FLAGS=-lm
CC_FLAGS=-Wall -g -fPIC 
ifeq ($(UNAME), Linux)
  LD_FLAGS+=-lrt 
endif

AR=ar
AR_FLAGS=-rsc

###############################################################################
# Compile rules
###############################################################################
SUBDIRS=benchmark \
        edit \
        gap_affine \
        gap_lineal \
        system \
        utils
       
LIB_WFA=$(FOLDER_BUILD)/libwfa.a
LIB_WFA_SO=$(FOLDER_BUILD)/libwfa.so

all: CC_FLAGS+=-O3
all: MODE=all
all: setup
all: $(SUBDIRS) lib_wfa lib_wfa_so tools

debug: MODE=all
debug: setup
debug: $(SUBDIRS) lib_wfa tools

setup:
	@mkdir -p $(FOLDER_BIN) $(FOLDER_BUILD)
	
lib_wfa: $(SUBDIRS)
	$(AR) $(AR_FLAGS) $(LIB_WFA) $(FOLDER_BUILD)/*.o 2> /dev/null

lib_wfa_so: LDFLAGS+=-shared -L.
lib_wfa_so: $(SUBDIRS)
	$(CC) $(LDFLAGS)  $(FOLDER_BUILD)/*.o -o $(LIB_WFA_SO) 

clean:
	rm -rf $(FOLDER_BIN) $(FOLDER_BUILD)
	
###############################################################################
# Subdir rule
###############################################################################
export
$(SUBDIRS):
	$(MAKE) --directory=$@ all
	
tools: $(SUBDIRS)
	$(MAKE) --directory=$@ $(MODE)

.PHONY: $(SUBDIRS) tools

