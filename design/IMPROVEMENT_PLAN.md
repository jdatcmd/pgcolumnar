# pgColumnar improvement plan (research-driven)

This plan proposes the next phase of pgColumnar work. It is derived entirely
from publicly readable academic literature on columnar storage and from the
current pgColumnar source and format spec. It names each technique, cites its
public source, states where pgColumnar stands today, and proposes concrete,
prioritized work. It preserves the clean-room discipline (see PROVENANCE.md and
REWRITE_PLAN.md): all techniques below come from the cited papers and the public
PostgreSQL API, never from any copyleft columnar project's source.

## Status

Implemented (format 2.1): I1 lightweight encodings (rle/for/delta), I2
compression-block run iterator, I3 compressed aggregate execution, I4 gorilla +
delta-of-delta, I5 dictionary encoding and adaptive selection, I6 vectorized
branch-free predicates, I7 per-chunk bloom filters, I8 late materialization.
Each landed with new differential/fuzz coverage and passes the full suite on
PostgreSQL 13-19. I9 (projections / Arrow interop) remains exploratory.

## Method and constraints

- **Sources.** The survey behind this plan (foundational and modern column-store
  papers) produced 15 high-confidence, independently verified findings. The key
  primary sources, all freely readable, are cited inline by short tag and listed
  in full at the end.
- **Format versioning.** Any on-disk change bumps the format minor version
  (currently 2.0, see FORMAT_AND_INTERFACE_SPEC.md section 3) and must keep read
  compatibility with existing 2.0 files. New encodings are additive: an old
  reader refuses an unknown code; a new reader still reads code 0 (none).
- **Test net.** The differential/recovery/fuzz suites (test/lib.sh and friends)
  are the regression net for everything here. They compare columnar output
  against a heap oracle across a broad type/null/boundary matrix and randomized
  rounds, which is exactly what value-stream encoding changes stress. Every item
  below lands with new differential cases before it is considered done.

## Where pgColumnar stands today

Confirmed from source (src/columnar_compression.c, columnar_reader.c,
columnar_vector.c, columnar_cache.c) and FORMAT_AND_INTERFACE_SPEC.md:

- **Compression is block-level only.** Each chunk's value stream is the raw
  serialized column values, then optionally run through a general-purpose codec
  (pglz/lz4/zstd) as a whole, with a store-verbatim fallback. There is no
  lightweight, type-aware, or order-aware encoding.
- **No compressed execution.** The reader fully decodes a chunk group into
  arrays; the vectorized aggregates and scan then run over decoded values.
  Operators never see the encoded form.
- **Data skipping is min/max only.** Per-chunk minimum_value/maximum_value drive
  chunk-group skipping. There are no bloom filters or richer zone maps, and a
  point lookup decodes a whole chunk group (the documented latency weakness).
- **Vectorization is partial.** Vectorized scan and count/sum/avg/min/max exist,
  but predicate evaluation is row-at-a-time (ColumnarVecRowPasses) and there are
  no branch-free/SIMD primitives.
- **One encoding choice per table.** Compression type/level is a per-table
  option; there is no per-chunk adaptive selection.

## Gap analysis

| Technique (source) | pgColumnar today | Gap |
| --- | --- | --- |
| Lightweight encodings: RLE, dictionary, bit-pack, FOR/delta [C-Store, Abadi-SIGMOD06] | block codec over raw stream | no encoding layer at all |
| Super-scalar PFOR / PFOR-DELTA / PDICT, branch-free [X100-Compress] | none | none |
| Float XOR + delta-of-delta timestamps [Gorilla] | none | none |
| Operate directly on compressed data [Abadi-SIGMOD06, thesis] | decode-then-compute | no compressed execution |
| Compression-block abstraction API [Abadi-SIGMOD06] | none | needed to avoid operator explosion |
| Per-chunk adaptive encoding selection [Abadi-SIGMOD06 decision tree] | one codec per table | no per-chunk choice |
| Vectorized, cache-sized, branch-free primitives [X100] | partial; row-at-a-time filter | vectorized predicates + SIMD |
| Late materialization / position lists [Abadi-ICDE07] | projection pushdown only | delayed reconstruction |
| Zone maps beyond min/max; bloom filters | min/max only | equality skipping, point-lookup |
| RAM-CPU cache compression (JIT decode at cache granularity) [X100-Compress] | decompressed-chunk cache | aligned; revisit granularity |
| C-Store projections (multiple sort orders); PAX/hybrid [C-Store, PAX] | single layout | optional, larger effort |
| Arrow/Parquet interop | none | optional export/interchange |

## Prioritized roadmap

Ordered by impact per unit effort, and by dependency. Tiers 1–2 are the core
value; tier 3 is higher-effort or optional. Each item notes impact, effort,
risk, and on-disk-format impact.

### Tier 1 — the encoding foundation (biggest win, protected by the new tests)

**I1. Lightweight encoding layer + first encodings (RLE, dictionary, delta/FOR,
bit-packing).**
Insert a type-aware encoding stage between the raw value stream and the
general-purpose block codec. Encode a chunk's values with the best lightweight
scheme, then optionally still block-compress. Start with integer/‑family types
(int2/4/8, date, timestamp) for FOR+bit-packing and delta; RLE for low-cardinality
runs; dictionary for low-distinct text/values.
- Source: [C-Store] four-encoding scheme; [Abadi-SIGMOD06] encodings and results.
- Impact: high (size and scan CPU). Effort: high. Risk: medium (format).
- Format: add a per-chunk `value_encoding_type` (and any encoding header) to
  columnar.chunk; bump to 2.1; readers ignore/none for old chunks.

**I2. Compression-block abstraction API.**
A small internal interface exposing block properties `isOneValue`,
`isValueSorted`, `isPosContig` plus an iterator (`getNext`/`asArray`) and block
info (`getSize`/`getStartValue`/`getEndPosition`). Operators consume this instead
of raw per-encoding details, so we avoid N encodings × M operators variants.
- Source: [Abadi-SIGMOD06] compression-block API.
- Impact: high (enables I3 cleanly). Effort: medium. Risk: low. Format: none.

**I3. Compressed execution for scan filters and aggregates.**
Using I2, run operators directly on encoded blocks: SUM over an RLE block as
value × run-length; COUNT/MIN/MAX on RLE/dictionary without materializing;
predicate evaluation on dictionary codes; skip whole single-value blocks. This is
the result that turns compression from a ~3x space win into a 3–10x speed win.
- Source: [Abadi-SIGMOD06] (SUM speedups 3.3x RLE, 10.3x bit-vector, 3.94x dict);
  [Abadi-thesis].
- Impact: high. Effort: medium (after I2). Risk: medium. Format: none.

### Tier 2 — specialization and adaptivity

**I4. Float and timestamp specialized encodings (Gorilla).**
XOR-against-previous for float4/float8 (zero XOR = 1 bit; else leading-zero and
meaningful-bit control fields); delta-of-delta variable-length for timestamps.
Large win for time-series / append-mostly analytic tables.
- Source: [Gorilla].
- Impact: high for time-series. Effort: medium. Risk: low. Format: new encoding
  codes under I1's scheme.

**I5. Per-chunk adaptive encoding selection.**
Sample each chunk's cardinality, run-length, and sortedness and pick the encoding
by a decision tree (RLE for avg run > ~4; dictionary for distinct < ~50k or
position-contiguous access; bit-vector for very low cardinality; LZ/none
otherwise). Store the chosen code per chunk (already provided by I1).
- Source: [Abadi-SIGMOD06] Figure 10 decision tree.
- Impact: medium-high. Effort: medium. Risk: low. Format: none beyond I1.

**I6. Vectorized, branch-free predicate evaluation.**
Replace row-at-a-time ColumnarVecRowPasses with primitives that evaluate a
predicate over a decoded (or encoded, via I2) array into a selection vector,
written to be auto-vectorizable (restrict-qualified, no data-dependent branches).
- Source: [X100] vector primitives and cycle-per-tuple results.
- Impact: medium-high. Effort: medium. Risk: low. Format: none.

### Tier 3 — skipping, materialization, and larger bets

**I7. Bloom-filter / richer zone maps per chunk group.**
Optional per-chunk bloom filter for equality predicates, and consider count of
distinct / null counts in zone maps. Directly mitigates the point-lookup
weakness (a fetch decodes a whole chunk group) by skipping non-matching groups
on equality.
- Source: general zone-map/data-skipping practice; complements [C-Store] skipping.
- Impact: medium (point lookups, equality scans). Effort: medium. Risk: low.
- Format: optional bloom stream per chunk; bump minor; additive.

**I8. Late materialization with position lists.**
Delay tuple reconstruction until after predicates and (where possible)
aggregation, carrying position lists instead of reconstructed rows.
- Source: [Abadi-ICDE07] materialization strategies (~3x on average).
- Impact: medium. Effort: medium-high. Risk: medium. Format: none.

**I9 (optional, larger). C-Store-style projections and/or Arrow interop.**
Multiple sort-order projections of the same columns for query-specific ordering,
or Arrow/Parquet export for interchange and external vectorized tools. Both are
substantial; list as exploratory.
- Source: [C-Store] projections; Arrow/Parquet ecosystem.
- Impact: workload-dependent. Effort: high. Risk: medium-high.

## Suggested sequencing

1. I2 (block API) and I1 (encoding layer) together — the API shapes the encoding
   interface. Land RLE + dictionary + FOR/bit-pack first.
2. I3 (compressed execution) on the encodings from I1, via I2.
3. I5 (adaptive selection) once several encodings exist.
4. I4 (Gorilla) for float/timestamp.
5. I6 (vectorized predicates) — independent; can proceed in parallel.
6. I7/I8 skipping and materialization.
7. I9 only if a workload justifies it.

Each step: implement from the cited paper, extend the differential type-matrix
and fuzz suites with cases that target the new encoding/operator, run the full
version matrix, and append to PROVENANCE.md.

## Sources

- **[C-Store]** Stonebraker et al., "C-Store: A Column-oriented DBMS," VLDB 2005.
  https://web.stanford.edu/class/cs345d-01/rl/cstore.pdf
- **[X100]** Boncz, Zukowski, Nes, "MonetDB/X100: Hyper-Pipelining Query
  Execution," CIDR 2005. http://cidrdb.org/cidr2005/papers/P19.pdf
- **[Abadi-SIGMOD06]** Abadi, Madden, Ferreira, "Integrating Compression and
  Execution in Column-Oriented Database Systems," SIGMOD 2006.
  https://doi.org/10.1145/1142473.1142548
- **[Abadi-ICDE07]** Abadi, Myers, DeWitt, Madden, "Materialization Strategies
  in a Column-Oriented DBMS," ICDE 2007.
  http://www.cs.umd.edu/~abadi/papers/abadiicde2007.pdf
- **[Abadi-thesis]** Abadi, "Query Execution in Column-Oriented Database
  Systems," PhD thesis, MIT 2008.
  http://www.cs.umd.edu/~abadi/papers/abadiphd.pdf
- **[X100-Compress]** Zukowski, Heman, Nes, Boncz, "Super-Scalar RAM-CPU Cache
  Compression," ICDE 2006 (PFOR, PFOR-DELTA, PDICT).
  https://paperhub.s3.amazonaws.com/7558905a56f370848a04fa349dd8bb9d.pdf
- **[Gorilla]** Pelkonen et al., "Gorilla: A Fast, Scalable, In-Memory Time
  Series Database," VLDB 2015. https://www.vldb.org/pvldb/vol8/p1816-teller.pdf
- Boncz lecture notes (survey framing):
  https://homepages.cwi.nl/~boncz/mimuw/boncz_mimuw.pdf
