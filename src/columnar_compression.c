/*-------------------------------------------------------------------------
 *
 * columnar_compression.c
 *		Value-stream compression codecs for pgColumnar (spec 5): none, pglz,
 *		lz4, and zstd. Each chunk's value stream is compressed independently.
 *		If a codec does not shrink the data, or the codec is not built into
 *		this binary, the raw bytes are stored with type "none" instead.
 *
 * pglz is always available (it ships with PostgreSQL). lz4 and zstd are
 * linked only when the system libraries were found at build time; when they
 * are compiled out, a request for them falls back to a codec that is present.
 *
 * Independent MIT implementation built from design/FORMAT_AND_INTERFACE_SPEC.md
 * (format 2.0) and the public PostgreSQL and system library APIs only.
 *
 *-------------------------------------------------------------------------
 */
#include "columnar.h"

#include "common/pg_lzcompress.h"

#ifdef HAVE_LIBLZ4
#include <lz4.h>
#endif

#ifdef HAVE_LIBZSTD
#include <zstd.h>
#endif

/*
 * ColumnarCodecAvailable
 *		Whether a compression type can be produced by this binary. none and
 *		pglz are always available; lz4 and zstd depend on the build.
 */
bool
ColumnarCodecAvailable(int compressionType)
{
	switch (compressionType)
	{
		case COLUMNAR_COMPRESSION_NONE:
		case COLUMNAR_COMPRESSION_PGLZ:
			return true;
		case COLUMNAR_COMPRESSION_LZ4:
#ifdef HAVE_LIBLZ4
			return true;
#else
			return false;
#endif
		case COLUMNAR_COMPRESSION_ZSTD:
#ifdef HAVE_LIBZSTD
			return true;
#else
			return false;
#endif
		default:
			return false;
	}
}

/*
 * try_pglz
 *		Compress with PostgreSQL's builtin pglz. Returns the compressed length
 *		on success (always < rawLen because we use the "always" strategy and
 *		reject non-shrinking output), or -1 to signal fallback.
 */
static int32
try_pglz(const char *raw, uint32 rawLen, char *dest)
{
	int32		clen;

	if (rawLen == 0 || rawLen > (uint32) INT32_MAX)
		return -1;

	clen = pglz_compress(raw, (int32) rawLen, dest, PGLZ_strategy_always);
	if (clen < 0 || (uint32) clen >= rawLen)
		return -1;

	return clen;
}

#ifdef HAVE_LIBLZ4
static int32
try_lz4(const char *raw, uint32 rawLen, char *dest, int destCap)
{
	int			clen;

	if (rawLen == 0 || rawLen > (uint32) LZ4_MAX_INPUT_SIZE)
		return -1;

	clen = LZ4_compress_default(raw, dest, (int) rawLen, destCap);
	if (clen <= 0 || (uint32) clen >= rawLen)
		return -1;

	return clen;
}
#endif

#ifdef HAVE_LIBZSTD
static int32
try_zstd(const char *raw, uint32 rawLen, char *dest, size_t destCap, int level)
{
	size_t		clen;

	if (rawLen == 0)
		return -1;

	clen = ZSTD_compress(dest, destCap, raw, (size_t) rawLen, level);
	if (ZSTD_isError(clen) || clen >= rawLen || clen > (size_t) INT32_MAX)
		return -1;

	return (int32) clen;
}
#endif

/*
 * ColumnarCompressValueStream
 *		Compress rawLen bytes at raw using the requested codec at the given
 *		level. On success at shrinking the data, returns a palloc'd buffer in
 *		*outData with its length in *outLen and the codec actually used in
 *		*usedType (with *usedLevel the level that codec used). If the codec is
 *		unavailable or does not shrink the data, returns a copy of the raw
 *		bytes with *usedType == COLUMNAR_COMPRESSION_NONE (spec 5).
 *
 *		The output is always allocated in the current memory context.
 */
void
ColumnarCompressValueStream(const char *raw, uint32 rawLen,
							int requestedType, int level,
							char **outData, uint32 *outLen,
							int *usedType, int *usedLevel)
{
	char	   *dest;
	int32		clen = -1;

	*usedType = COLUMNAR_COMPRESSION_NONE;
	*usedLevel = 0;

	/* An empty stream (all-null chunk) is stored verbatim. */
	if (rawLen == 0)
	{
		*outData = NULL;
		*outLen = 0;
		return;
	}

	switch (requestedType)
	{
		case COLUMNAR_COMPRESSION_PGLZ:
			{
				dest = palloc(PGLZ_MAX_OUTPUT(rawLen));
				clen = try_pglz(raw, rawLen, dest);
				if (clen >= 0)
				{
					*outData = dest;
					*outLen = (uint32) clen;
					*usedType = COLUMNAR_COMPRESSION_PGLZ;
					*usedLevel = 0;
					return;
				}
				pfree(dest);
				break;
			}

		case COLUMNAR_COMPRESSION_LZ4:
#ifdef HAVE_LIBLZ4
			{
				int			cap = LZ4_compressBound((int) rawLen);

				if (cap > 0)
				{
					dest = palloc(cap);
					clen = try_lz4(raw, rawLen, dest, cap);
					if (clen >= 0)
					{
						*outData = dest;
						*outLen = (uint32) clen;
						*usedType = COLUMNAR_COMPRESSION_LZ4;
						*usedLevel = 0;
						return;
					}
					pfree(dest);
				}
			}
#endif
			break;

		case COLUMNAR_COMPRESSION_ZSTD:
#ifdef HAVE_LIBZSTD
			{
				size_t		cap = ZSTD_compressBound((size_t) rawLen);

				if (!ZSTD_isError(cap))
				{
					dest = palloc(cap);
					clen = try_zstd(raw, rawLen, dest, cap, level);
					if (clen >= 0)
					{
						*outData = dest;
						*outLen = (uint32) clen;
						*usedType = COLUMNAR_COMPRESSION_ZSTD;
						*usedLevel = level;
						return;
					}
					pfree(dest);
				}
			}
#endif
			break;

		case COLUMNAR_COMPRESSION_NONE:
		default:
			break;
	}

	/*
	 * Fallback: the requested codec was unavailable or did not help. Store the
	 * raw bytes uncompressed (spec 5). We copy so the caller owns a buffer with
	 * uniform ownership semantics.
	 */
	dest = palloc(rawLen);
	memcpy(dest, raw, rawLen);
	*outData = dest;
	*outLen = rawLen;
	*usedType = COLUMNAR_COMPRESSION_NONE;
	*usedLevel = 0;
}

/*
 * ColumnarDecompressValueStream
 *		Decompress compLen bytes at comp of the given codec into a fresh buffer
 *		of rawLen bytes allocated in targetContext, and return it. For type
 *		none the bytes are copied. Errors out if the codec is not built in or
 *		the decoded length does not match rawLen.
 */
char *
ColumnarDecompressValueStream(const char *comp, uint32 compLen,
							  int compressionType, uint32 rawLen,
							  MemoryContext targetContext)
{
	char	   *dest;

	if (rawLen == 0)
		return NULL;

	dest = MemoryContextAlloc(targetContext, rawLen);

	switch (compressionType)
	{
		case COLUMNAR_COMPRESSION_NONE:
			if (compLen != rawLen)
				elog(ERROR,
					 "columnar: uncompressed stream length %u does not match "
					 "expected %u", compLen, rawLen);
			memcpy(dest, comp, rawLen);
			break;

		case COLUMNAR_COMPRESSION_PGLZ:
			{
				int32		dlen = pglz_decompress(comp, (int32) compLen, dest,
												   (int32) rawLen, true);

				if (dlen != (int32) rawLen)
					elog(ERROR,
						 "columnar: pglz decompression produced %d bytes, "
						 "expected %u", dlen, rawLen);
				break;
			}

		case COLUMNAR_COMPRESSION_LZ4:
#ifdef HAVE_LIBLZ4
			{
				int			dlen = LZ4_decompress_safe(comp, dest, (int) compLen,
													   (int) rawLen);

				if (dlen != (int) rawLen)
					elog(ERROR,
						 "columnar: lz4 decompression produced %d bytes, "
						 "expected %u", dlen, rawLen);
				break;
			}
#else
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("columnar: this build was compiled without lz4 "
							"support, but a chunk requires it")));
#endif
			break;

		case COLUMNAR_COMPRESSION_ZSTD:
#ifdef HAVE_LIBZSTD
			{
				size_t		dlen = ZSTD_decompress(dest, (size_t) rawLen, comp,
												   (size_t) compLen);

				if (ZSTD_isError(dlen) || dlen != (size_t) rawLen)
					elog(ERROR,
						 "columnar: zstd decompression failed or produced the "
						 "wrong length (expected %u)", rawLen);
				break;
			}
#else
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("columnar: this build was compiled without zstd "
							"support, but a chunk requires it")));
#endif
			break;

		default:
			elog(ERROR, "columnar: unknown compression type %d", compressionType);
	}

	return dest;
}
