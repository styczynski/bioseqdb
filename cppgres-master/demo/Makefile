MODULES = cppgres_demo
EXTENSION = cppgres_demo
DATA = cppgres_demo--1.sql cppgres_demo--1.control

CUSTOM_CC=c++
PG_CXXFLAGS=-I $(shell pwd)/../src --std=c++20

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
