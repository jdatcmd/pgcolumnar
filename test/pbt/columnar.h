/*
 * Stub columnar.h for the standalone codec property test (test/pbt). It exposes
 * only the encoding codes, the block-reader struct, and the codec prototypes
 * that columnar_encoding.c needs, over the minimal PG shim. The real header is
 * src/columnar.h; this one is picked up only when compiling the codec source
 * from the test/pbt build directory.
 */
#ifndef PGCOLUMNAR_PBT_COLUMNAR_H
#define PGCOLUMNAR_PBT_COLUMNAR_H

#include "pgstub.h"

#define COLUMNAR_ENCODING_NONE 0
#define COLUMNAR_ENCODING_RLE 1
#define COLUMNAR_ENCODING_FOR 2
#define COLUMNAR_ENCODING_DELTA 3
#define COLUMNAR_ENCODING_GORILLA 4
#define COLUMNAR_ENCODING_DOD 5
#define COLUMNAR_ENCODING_DICT 6
#define COLUMNAR_ENCODING_ALP 7

#ifndef PG_UINT32_MAX
#define PG_UINT32_MAX 0xFFFFFFFFU
#endif

typedef struct ColumnarBlockReader
{
	const char *raw;
	uint64		valueCount;
	int			width;
	uint64		pos;
} ColumnarBlockReader;

extern int ColumnarEncodeChunk(const char *raw, uint32 rawLen,
							   Form_pg_attribute att, uint64 valueCount,
							   char **out, uint32 *outLen);
extern char *ColumnarDecodeChunk(const char *enc, uint32 encLen,
								 int encodingType, Form_pg_attribute att,
								 uint64 valueCount, uint32 rawLen,
								 MemoryContext cx);
extern const char *ColumnarEncodingName(int encodingType);
extern void ColumnarBlockReaderInit(ColumnarBlockReader *br, const char *raw,
									uint64 valueCount, int width);
extern bool ColumnarBlockNextRun(ColumnarBlockReader *br,
								 const char **valBytes, uint64 *runLen);

#endif							/* PGCOLUMNAR_PBT_COLUMNAR_H */
