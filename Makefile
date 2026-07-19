# pgColumnar - PGXS build
# Independent MIT implementation of the columnar table access method.

MODULE_big = columnar

OBJS = \
	src/columnar_tableam.o \
	src/columnar_storage.o \
	src/columnar_metadata.o \
	src/columnar_write_state.o \
	src/columnar_compression.o \
	src/columnar_reader.o \
	src/columnar_row_mask.o \
	src/columnar_customscan.o \
	src/columnar_cache.o \
	src/columnar_vector.o \
	src/columnar_vacuum.o

EXTENSION = columnar
DATA = columnar--1.0.sql
PGFILEDESC = "pgColumnar - column-oriented table access method"

REGRESS =

# Optional compression codecs. lz4 and zstd are linked when the system
# development libraries are present (detected with pkg-config); otherwise
# those codecs are compiled out cleanly and requests for them fall back to a
# codec that is built in (spec 5). pglz is always available from PostgreSQL.
PKG_CONFIG ?= pkg-config

ifeq ($(shell $(PKG_CONFIG) --exists liblz4 && echo yes),yes)
PG_CPPFLAGS += -DHAVE_LIBLZ4 $(shell $(PKG_CONFIG) --cflags liblz4)
SHLIB_LINK += $(shell $(PKG_CONFIG) --libs liblz4)
endif

ifeq ($(shell $(PKG_CONFIG) --exists libzstd && echo yes),yes)
PG_CPPFLAGS += -DHAVE_LIBZSTD $(shell $(PKG_CONFIG) --cflags libzstd)
SHLIB_LINK += $(shell $(PKG_CONFIG) --libs libzstd)
endif

PG_CONFIG ?= pg_config

# Select the C standard by PostgreSQL major version. PostgreSQL 13 through 18
# are written to compile as C17 (their headers predate C23), so pin gnu17 there
# for a deterministic build regardless of the compiler's default. PostgreSQL 19
# uses C23 constructs in its headers (for example typeof_unqual in nodes.h), so
# it needs gnu23. The value is appended to CFLAGS through PG_CFLAGS, which PGXS
# honors.
PG_MAJORVERSION := $(shell $(PG_CONFIG) --version | sed -E 's/^[^0-9]*([0-9]+).*/\1/')
ifeq ($(shell test "$(PG_MAJORVERSION)" -ge 19 && echo yes),yes)
PG_CFLAGS += -std=gnu23
else
PG_CFLAGS += -std=gnu17
endif

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
