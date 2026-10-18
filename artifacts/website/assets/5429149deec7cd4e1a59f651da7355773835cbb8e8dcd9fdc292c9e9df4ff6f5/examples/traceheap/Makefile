EXTENSION = traceheap
MODULE_big = traceheap
OBJS = traceheap.o
DATA = traceheap--1.0.sql
PG_CONFIG ?= pg_config
PG_CXXFLAGS += -std=c++17 -fno-exceptions -fno-rtti
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
