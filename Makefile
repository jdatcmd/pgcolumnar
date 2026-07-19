# pgColumnar - PGXS build
# Independent MIT implementation of the columnar table access method.

MODULE_big = columnar

OBJS = \
	src/columnar_tableam.o \
	src/columnar_storage.o \
	src/columnar_metadata.o \
	src/columnar_write_state.o \
	src/columnar_reader.o

EXTENSION = columnar
DATA = columnar--1.0.sql
PGFILEDESC = "pgColumnar - column-oriented table access method"

REGRESS =

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
