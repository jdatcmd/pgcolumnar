# Gap 22: intra-group late materialization (position lists)

Status: planned. Tier: performance. Format change: none.

## Motivation

I8 added late materialization at group granularity: the scan decodes the
predicate columns, builds the selection vector, and decodes the output columns
only when *some* row in the group survives. It does not yet avoid decoding output
values for the *non-surviving rows within* a surviving group. For a filter that
keeps, say, 2% of a group's rows, we still decode 100% of the output columns for
that group. Carrying a position list of survivors and gathering only those output
values is the standard late-materialization win (Abadi, ICDE 2007: ~3x average).

## Current state

- `columnar_reader.c`: `columnar_decode_group_columns` decodes a column by
  walking the value stream sequentially (`ColumnarDecodeValue` per present row).
  `ColumnarDecodeGroupColumns` decodes a whole column for the group.
- `columnar_customscan.c`: `ColumnarScanNextVector` computes `vecSel[]` then
  emits surviving rows, reading `vec.values[c][i]` for all output columns (which
  were fully decoded).

## Design

Add a position-gather decode for output columns:

- `ColumnarDecodeColumnAtPositions(readState, c, positions[], npos, vec)`:
  decode only the values at the given row positions of the group. For a
  fixed-width, no-null, uncompressed-none chunk this is a direct indexed read
  (`base + pos*width`); for compressed/encoded chunks and nullable columns, walk
  the stream but materialize a Datum only at requested positions (still O(rows)
  to walk, but O(npos) Datums and O(npos) detoast/copy for varlena, which is the
  real cost for wide output columns).
- The scan: after `ColumnarVecSelect`, build the position list from `vecSel`; if
  selectivity is below a threshold (e.g. < 50%), decode output columns by
  position; otherwise decode the whole column (the current path) since gather has
  overhead when most rows survive.
- Encodings that support random access without a full walk (FOR, dict, none over
  fixed width) can gather in O(npos); RLE/delta/DoD/gorilla need a walk to reach
  a position (sequential dependence), so for those, gather still walks but skips
  Datum construction for non-selected positions.

## On-disk / API impact

None on disk. New reader entry point; scan chooses gather vs full decode by
selectivity. Gate with `columnar.enable_late_materialization` (reuse) or a sub-
threshold GUC `columnar.late_materialization_selectivity`.

## Testing

Differential: selective filters (point, narrow range, no-match) over wide tables
with by-value and varlena output columns, on and off, versus the heap oracle
(I8's cases extended). Correctness is the property; the win is fewer decodes.

## Effort / risk

Medium. Risk: the random-access gather for each encoding must return exactly what
the sequential decode would (esp. nulls via the exists stream, and varlena
sizing). Contained by the oracle.

## Source

Abadi, Myers, DeWitt, Madden, "Materialization Strategies in a Column-Oriented
DBMS," ICDE 2007.
