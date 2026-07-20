/*
 * Minimal PostgreSQL shim for property-testing the value-stream codecs
 * (columnar_encoding.c) as a standalone C program, with no PostgreSQL backend.
 * It provides just the types, StringInfo, palloc, varlena, and error macros the
 * codec functions use, so the same source compiles and runs under a normal C
 * compiler and a property-test driver. Not a PostgreSQL header; used only by the
 * test harness under test/pbt.
 */
#ifndef PGCOLUMNAR_PBT_PGSTUB_H
#define PGCOLUMNAR_PBT_PGSTUB_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef int16_t int16;
typedef int32_t int32;
typedef int64_t int64;
typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;
typedef size_t Size;
typedef unsigned int Oid;
#define InvalidOid ((Oid) 0)

/* memory: the codecs allocate output buffers; a leak in a test process is fine */
typedef void *MemoryContext;
#define palloc(sz) malloc(sz)
#define palloc0(sz) calloc(1, (sz))
#define pfree(p) free(p)
#define MemoryContextAlloc(cx, sz) malloc(sz)

/* error path: only the unknown-encoding branch calls ereport; abort loudly */
#define ereport(elevel, ...) do { fprintf(stderr, "ereport called\n"); abort(); } while (0)
#define errcode(x) 0
#define errmsg(...) 0

/* attribute: the codecs read attlen, attbyval, atttypid only */
typedef struct FormData_pg_attribute
{
	int16		attlen;
	bool		attbyval;
	Oid			atttypid;
} FormData_pg_attribute;
typedef FormData_pg_attribute *Form_pg_attribute;

/*
 * varlena: dict encoding sizes values with VARSIZE_ANY. The test builds varlena
 * values with a 4-byte total-length header (no flag bits), so a plain 32-bit
 * read is the size, matching how the codec round-trips the bytes.
 */
#define VARSIZE_ANY(p) (*((const uint32 *) (p)))

/* StringInfo: a growable byte buffer */
typedef struct StringInfoData
{
	char	   *data;
	int			len;
	int			maxlen;
} StringInfoData;
typedef StringInfoData *StringInfo;

static inline void
initStringInfo(StringInfo s)
{
	s->maxlen = 64;
	s->data = (char *) malloc(s->maxlen);
	s->len = 0;
	s->data[0] = '\0';
}

static inline void
enlargeStringInfo(StringInfo s, int needed)
{
	if (s->len + needed + 1 > s->maxlen)
	{
		while (s->len + needed + 1 > s->maxlen)
			s->maxlen *= 2;
		s->data = (char *) realloc(s->data, s->maxlen);
	}
}

static inline void
appendBinaryStringInfo(StringInfo s, const char *data, int datalen)
{
	enlargeStringInfo(s, datalen);
	memcpy(s->data + s->len, data, datalen);
	s->len += datalen;
	s->data[s->len] = '\0';
}

static inline void
appendStringInfoChar(StringInfo s, char ch)
{
	enlargeStringInfo(s, 1);
	s->data[s->len++] = ch;
	s->data[s->len] = '\0';
}

#endif							/* PGCOLUMNAR_PBT_PGSTUB_H */
