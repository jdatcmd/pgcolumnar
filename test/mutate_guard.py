#!/usr/bin/env python3
#
# Guard-mutation harness for the Parquet page reader.
#
# A crafted-file test that passes proves nothing on its own: the file may be
# rejected by some other check, or never have been built at all. Both happened
# while writing test/native_parquet_streaming.sh. This script deletes ONE guard
# from a copy of the tree so the suite can be run against a build that is missing
# exactly that guard; the corresponding check must then FAIL.
#
# It refuses to run if the target text is not found, because silently testing an
# unmutated build is the failure mode it exists to prevent (that also happened).
#
# Usage, from a container with the tree copied to /root/mut:
#
#     rm -rf /root/mut && cp -a <src> /root/mut
#     python3 test/mutate_guard.py progress     # or: size, offset
#     cd /root/mut && make ... && make install ...
#     PGC_SKIP_BUILD=1 bash test/native_parquet_streaming.sh <pg_config>
#
# Expected: the check named for that guard fails, and only that one.
#
#     progress -> "zero-filled page area is rejected, not walked byte by byte"
#     size     -> "page size past end of file is rejected by the size bound"
#     offset   -> "negative page offset is rejected by the offset check"
#     onedict  -> "a second dictionary page in a chunk is rejected"
#
# v2levels is deliberately not in that list. No behavioural check separates it:
# with the guard removed the crafted file is still rejected with the same message
# (the level decode fails on garbage, and a negative compressed_size - levLen
# becomes a huge size_t so valbuf + vallen wraps and bounds checks fail closed).
# An ASAN build does not separate it either, because palloc sub-allocates from a
# larger block and the read past a 32-byte page stays inside it. A PostgreSQL
# built with USE_VALGRIND would show it. The guard is argued from the code.
#
import os
import sys

which = sys.argv[1]
p = os.environ.get('PGC_MUT_SRC', '/root/mut/src/columnar_parquet_reader.c')
s = open(p).read()

MUT = {
  # the progress guard
  'progress': ("""		else if (h.num_values <= 0)
			return false;""", "\t\telse if (false)\n\t\t\treturn false;\t/* MUTATED: progress guard removed */"),
  # the per-page compressed_size bound
  'size': ("""		if (h.compressed_size < 0 ||
			(int64) h.compressed_size > src->len - pos - (int64) hdrlen)
			return false;""", "\t\t/* MUTATED: size bound removed */"),
  # the page-offset-in-file check
  'onedict': ("""		if (h.type == PQ_PAGE_DICTIONARY)
		{
			if (++nDictPages > 1)
				return false;
		}
		else if (h.num_values <= 0)""",
              "\t\tif (h.type != PQ_PAGE_DICTIONARY && h.num_values <= 0)"),
  'v2levels': ("""		if (h.is_v2 &&
			(h.def_levels_len < 0 || h.rep_levels_len < 0 ||
			 (int64) h.def_levels_len + (int64) h.rep_levels_len >
			 (int64) h.compressed_size))
			return false;""", "\t\t/* MUTATED: v2 level bound removed */"),
  'offset': ("""	if (off < 0 || off >= src->len)
		return false;""",
             "\tif (false)\n\t\treturn false;\t/* MUTATED: offset check removed */"),
}
old, new = MUT[which]
if old not in s:
    sys.exit(f"MUTATION TARGET FOR {which} NOT FOUND -- refusing to test an unmutated build")
open(p, 'w').write(s.replace(old, new, 1))
print(f"mutated: {which}")
