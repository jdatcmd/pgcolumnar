#!/usr/bin/env bash
#
# pgColumnar Parquet FIXED_LEN_BYTE_ARRAY read coverage (Phase G).
#
# The reader advertises uuid and numeric(p,s) for FLBA columns via
# parquet_schema(), and the exporter writes both. This suite covers the read side
# that made those advertisements truthful: uuid as a 16-byte FLBA, and DECIMAL as
# a big-endian two's-complement integer at the column's scale, in either a fixed
# (FLBA) or variable (BYTE_ARRAY) width. It rounds our own export back through the
# reader and also reads pyarrow-written files, and checks correctness against a
# heap oracle.
#
# Usage:  test/native_parquet_flba.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

have_pyarrow=0
python3 -c 'import pyarrow.parquet' 2>/dev/null && have_pyarrow=1

W="$PGC_WORKDIR"
psql_run "CREATE SERVER pq FOREIGN DATA WRAPPER pgcolumnar_parquet;"

# ---- round-trip our own exporter (uuid + numeric via FLBA) -----------------
psql_run "CREATE TABLE src (
            id int,
            u  uuid,
            d  numeric(12,4),
            dn numeric(12,4),
            fb bytea)
          USING pgcolumnar;"
psql_run "INSERT INTO src VALUES
            (1, '11111111-2222-3333-4444-555555555555', 123.4500, -0.0001, '\\xdeadbeef'),
            (2, 'ffffffff-ffff-ffff-ffff-ffffffffffff', -9999.9999, 0, '\\x00'),
            (3, '00000000-0000-0000-0000-000000000000', 0.0000, 42.0, NULL);"

EXP="$W/roundtrip.parquet"
psql_run "SELECT pgcolumnar.export_parquet('src'::regclass, '$EXP');"

# read_parquet must return exactly what the heap holds
check "uuid round-trips through export/read" \
	"$(pgc_set_hash "SELECT id, u FROM pgcolumnar.read_parquet('$EXP') AS t(id int, u uuid, d numeric(12,4), dn numeric(12,4), fb bytea)")" \
	"$(pgc_set_hash "SELECT id, u FROM src")"
check "numeric round-trips through export/read" \
	"$(pgc_set_hash "SELECT id, d, dn FROM pgcolumnar.read_parquet('$EXP') AS t(id int, u uuid, d numeric(12,4), dn numeric(12,4), fb bytea)")" \
	"$(pgc_set_hash "SELECT id, d, dn FROM src")"
check "whole-row round-trip == source" \
	"$(pgc_set_hash "SELECT * FROM pgcolumnar.read_parquet('$EXP') AS t(id int, u uuid, d numeric(12,4), dn numeric(12,4), fb bytea)")" \
	"$(pgc_set_hash "SELECT * FROM src")"

# import_parquet shares the decoder
psql_run "CREATE TABLE imp (id int, u uuid, d numeric(12,4), dn numeric(12,4), fb bytea);"
psql_run "SELECT pgcolumnar.import_parquet('imp'::regclass, '$EXP');"
check "import_parquet == source" \
	"$(pgc_set_hash "SELECT * FROM imp")" "$(pgc_set_hash "SELECT * FROM src")"

# FDW surface
psql_run "CREATE FOREIGN TABLE ft (id int, u uuid, d numeric(12,4), dn numeric(12,4), fb bytea)
          SERVER pq OPTIONS (path '$EXP');"
check "FDW scan == source" \
	"$(pgc_set_hash "SELECT * FROM ft")" "$(pgc_set_hash "SELECT * FROM src")"

# ---- pyarrow-written files (foreign producer) ------------------------------
if [ "$have_pyarrow" = 1 ]; then
	python3 - "$W" <<'PY'
import sys, uuid, decimal, pyarrow as pa, pyarrow.parquet as pq
W = sys.argv[1]
us = [uuid.UUID('11111111-2222-3333-4444-555555555555').bytes,
      uuid.UUID('00000000-0000-0000-0000-000000000000').bytes]
pq.write_table(pa.table({"u": pa.array(us, pa.binary(16))}), f"{W}/pa_uuid.parquet")
# decimal128 -> FLBA; a range that stresses sign and scale
ds = [decimal.Decimal("123.45"), decimal.Decimal("-9999.99"),
      decimal.Decimal("0.01"), decimal.Decimal("0.00")]
pq.write_table(pa.table({"d": pa.array(ds, pa.decimal128(10, 2))}), f"{W}/pa_dec.parquet")
PY
	# uuid stored as raw bytes; declare bytea to compare, since pyarrow gives no
	# uuid logical type, then also read as uuid (our exporter's convention).
	check "pyarrow uuid reads as uuid" \
		"$(q "SELECT u::text FROM pgcolumnar.read_parquet('$W/pa_uuid.parquet') AS t(u uuid) ORDER BY u LIMIT 1;")" \
		"00000000-0000-0000-0000-000000000000"
	check "pyarrow decimal128 values are exact" \
		"$(pgc_set_hash "SELECT d FROM pgcolumnar.read_parquet('$W/pa_dec.parquet') AS t(d numeric(10,2))")" \
		"$(pgc_set_hash "SELECT * FROM (VALUES (123.45::numeric(10,2)),(-9999.99),(0.01),(0.00)) v(d)")"

	# ---- crafted out-of-range scale must be rejected, not decoded -----------
	# The DECIMAL scale comes straight from the footer, and left unvalidated it
	# drives pq_decimal_to_numeric's zero-fill (~scale bytes) into a fixed stack
	# buffer: a large scale is a stack smash on a crafted file. Rewrite the schema
	# element's scale field (SchemaElement field 7) in a decimal128(10,2) file from
	# 2 to 50 -- out of range (> precision, > 38) but small enough that the
	# unguarded decoder does NOT crash: it decodes a (wrong) value with no error.
	# The guard must instead reject the bind. zigzag(2)=0x04 and zigzag(50)=0x64 are
	# both one byte, so the edit is in place and the footer stays well-formed.
	python3 - "$W" <<'PY'
import sys, decimal, pyarrow as pa, pyarrow.parquet as pq
W = sys.argv[1]
pq.write_table(pa.table({"d": pa.array([decimal.Decimal("1.23")], pa.decimal128(10, 2))}),
               f"{W}/dec_badscale.parquet")
raw = bytearray(open(f"{W}/dec_badscale.parquet", "rb").read())
flen = int.from_bytes(raw[-8:-4], "little")
pos = len(raw) - 8 - flen

def varint():
    global pos
    sh = 0; out = 0
    while True:
        b = raw[pos]; pos += 1
        out |= (b & 0x7F) << sh
        if not b & 0x80: return out
        sh += 7
def zigzag():
    u = varint(); return (u >> 1) ^ -(u & 1)
def field(last):
    global pos
    b = raw[pos]; pos += 1
    if b == 0: return 0, 0, last
    t = b & 0x0F; d = b >> 4
    fid = last + d if d else zigzag()
    return t, fid, fid
def skip(t):
    global pos
    if t in (1, 2): return
    if t == 3: pos += 1
    elif t in (4, 5, 6): zigzag()
    elif t == 7: pos += 8
    elif t == 8:
        blen = varint()      # advance past the length bytes, then the content;
        pos += blen          # `pos += varint()` would drop the length advance
    elif t in (9, 10):
        b = raw[pos]; pos += 1
        n = (b >> 4) & 0x0F; et = b & 0x0F
        if n == 0x0F: n = varint()
        for _ in range(n): skip(et)
    elif t == 12:
        last = 0
        while True:
            ft, _f, last = field(last)
            if ft == 0: break
            skip(ft)

# FileMetaData field 2 is the schema list<SchemaElement>; the scale (field 7) sits
# on the leaf element. Walk to it and patch the one byte in place.
patched = False
last = 0
while not patched:
    ft, fid, last = field(last)
    if ft == 0: break
    if fid == 2 and ft in (9, 10):
        b = raw[pos]; pos += 1
        n = (b >> 4) & 0x0F; et = b & 0x0F
        if n == 0x0F: n = varint()
        for _ in range(n):
            elast = 0
            while True:
                eft, efid, elast = field(elast)
                if eft == 0: break
                if efid == 7 and eft in (5, 6, 4):
                    assert raw[pos] == 0x04, hex(raw[pos])   # zigzag(2)
                    raw[pos] = 0x64                           # zigzag(50)
                    patched = True
                    break
                skip(eft)
            if patched: break
        break
    skip(ft)
assert patched, "did not find a scale field to patch"
open(f"{W}/dec_badscale.parquet", "wb").write(bytes(raw))
print("  patched scale 2 -> 50", file=sys.stderr)
PY

	psql_run "CREATE FUNCTION pgc_try_flba(q text) RETURNS text LANGUAGE plpgsql AS \$\$
	          BEGIN EXECUTE q; RETURN 'NO ERROR';
	          EXCEPTION WHEN OTHERS THEN RETURN 'REJECTED'; END \$\$;"
	# Fixed: the bind guard rejects scale > precision/38 -> REJECTED. Unguarded: the
	# decoder accepts it and returns a wrong-but-valid numeric -> NO ERROR. So this
	# check distinguishes fixed from unfixed without depending on a crash.
	check "crafted out-of-range scale is rejected, not decoded" \
		"$(q "SELECT pgc_try_flba(\$q\$SELECT * FROM pgcolumnar.read_parquet('$W/dec_badscale.parquet') AS t(d numeric)\$q\$);")" \
		"REJECTED"
	check "backend survived the crafted scale" "$(q 'SELECT 1;')" "1"

	# ---- DECIMAL held in an INT32 or INT64 ----------------------------------
	#
	# The spec allows a DECIMAL to be stored as a plain integer, which is what
	# writers use for small precisions, and pyarrow does it on request. Verified
	# in this pyarrow (23.0.1) rather than assumed from the spec's thresholds:
	# precision up to 9 becomes INT32, up to 18 becomes INT64, beyond that FLBA.
	# Unlike the byte-array forms the integer is little-endian, so the decode
	# widens it into a big-endian buffer and reuses the one scale conversion.
	python3 - "$W" <<'PYINT'
import decimal, os, sys
import pyarrow as pa, pyarrow.parquet as pq
W = sys.argv[1]
vals = [decimal.Decimal("1.25"), decimal.Decimal("-3.50"),
        decimal.Decimal("0.00"), None]
for name, p, s in (("i32", 9, 2), ("i64", 18, 6)):
    t = pa.table({"d": pa.array([None if v is None else v.scaleb(0) for v in vals],
                                pa.decimal128(p, s))})
    pq.write_table(t, os.path.join(W, f"dec_{name}.parquet"),
                   store_decimal_as_integer=True, compression="none")
phys = {}
for name in ("i32", "i64"):
    f = pq.ParquetFile(os.path.join(W, f"dec_{name}.parquet"))
    phys[name] = f.metadata.row_group(0).column(0).physical_type
if phys != {"i32": "INT32", "i64": "INT64"}:
    sys.exit("unexpected physical types: %s" % phys)
PYINT
	if [ $? -ne 0 ]; then
		echo "SKIP  this pyarrow does not store decimals as integers as expected"
	else
		check "INT32-backed DECIMAL reads" \
			"$(q "SELECT string_agg(d::text, ',' ORDER BY d) FROM pgcolumnar.read_parquet('$W/dec_i32.parquet') AS t(d numeric);")" \
			"-3.50,0.00,1.25"
		check "INT64-backed DECIMAL reads" \
			"$(q "SELECT string_agg(d::text, ',' ORDER BY d) FROM pgcolumnar.read_parquet('$W/dec_i64.parquet') AS t(d numeric);")" \
			"-3.500000,0.000000,1.250000"
		check "INT32-backed DECIMAL keeps its null" \
			"$(q "SELECT count(*) - count(d) FROM pgcolumnar.read_parquet('$W/dec_i32.parquet') AS t(d numeric);")" "1"
		check "parquet_schema advises numeric for an INT32 DECIMAL" \
			"$(q "SELECT data_type FROM pgcolumnar.parquet_schema('$W/dec_i32.parquet');")" \
			"numeric(9,2)"
		check "parquet_schema advises numeric for an INT64 DECIMAL" \
			"$(q "SELECT data_type FROM pgcolumnar.parquet_schema('$W/dec_i64.parquet');")" \
			"numeric(18,6)"
		# the integer reading is not silently reinterpreted: declaring bigint
		# against a DECIMAL column must still bind to the raw integer
		check "an INT64 DECIMAL still binds to bigint as the unscaled integer" \
			"$(q "SELECT string_agg(d::text, ',' ORDER BY d) FROM pgcolumnar.read_parquet('$W/dec_i64.parquet') AS t(d bigint);")" \
			"-3500000,0,1250000"
	fi
else
	echo "SKIP  pyarrow not available; foreign-producer FLBA cases skipped"
fi

pgc_summary
