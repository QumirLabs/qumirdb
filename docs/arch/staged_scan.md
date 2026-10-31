# Staged scan

A filter directly above a Parquet scan runs inside the scan task. The scan
reads the filter's columns in stages: each stage decodes its new columns only
for the rows the earlier stages keep, then evaluates its conjuncts on them.
The columns needed only above the filter, the payload, come last, for the rows
the whole filter keeps. This is the pipelined late materialization of column
stores described by Abadi et al. in
[Materialization Strategies in a Column-Oriented DBMS (ICDE 2007)](https://doi.org/10.1109/ICDE.2007.367892):
the positions that pass one predicate drive the scan of the next column.

```sql
SELECT SearchPhrase, MIN(URL), MIN(Title), COUNT(*), COUNT(DISTINCT UserID)
FROM hits
WHERE Title LIKE '%Google%' AND URL NOT LIKE '%.google.%' AND SearchPhrase <> ''
GROUP BY SearchPhrase;
-- stages: [SearchPhrase, UserID] [Title] [URL]
```

## Stages

Only a wide column is read in a later stage: one of more than 16 bytes per row
uncompressed in the file metadata, wider than any fixed-width value. A narrow
column, an integer, a date, a decimal or a short string, gains nothing from a
sparse read: its pages are decompressed either way and its values cost as much
to skip as to copy. Stage 0 reads every narrow column the scan needs, filter
or payload. The stages come from the conjunct order of
[optimizations.md 4.11](optimizations.md#411-conjunct-order-in-a-filter-over-a-scan)
with the narrow columns counted as free: the conjuncts over narrow columns run
in stage 0, then each wide column in the order of its cost.

- Stage 0 is read densely with Arrow, as an ordinary scan, and sets the batch.
- A later stage is read with the Parquet column reader. A page without a row
  the selection keeps is skipped before decompression, a long run of rejected
  rows inside a page is skipped, and only the kept strings are copied.
- A batch in which more than half of the rows pass is read densely: a sparse
  read would save little copying and still check every row.
- A batch no row of which passes is not emitted; the later columns skip its
  rows.

A page that reaches past the batch is read, as the next batch may need it. On
ClickBench `hits` a URL page holds about 13,000 rows: in q21, 13% of the rows
pass `SearchPhrase <> ''` and they still touch almost every URL page, so the
saving there is the copy; in q22 the rows left after `Title LIKE` touch 27% of
the URL pages.

The staged scan applies to a chain of filters over one Parquet source when a
wide column is left after stage 0, to filter or to carry, and the scan has at
least as many lanes as there are workers. Otherwise the filter stays a task
after the scan. Fusing gives up the overlap of the scan and filter tasks of a
lane: while lanes are fewer than workers, that overlap is part of the query's
parallelism, and a staged scan of a small dimension table measured slower for
the same CPU time. `[scan] staged_filter` turns it off. The exported browser
plan keeps the separate filter, as its sources are read in JavaScript. Runtime
filters of the source stay tasks after the scan and see its selection.

## Kernels and the batch contract

Each stage is an ordinary filter kernel over the whole scan schema. It runs
with the previous stage's selection as its input selection, reads only its
own columns and does not evaluate a rejected row. The kernel ABI is unchanged:
the computation stays in generated kernels, and the scan task only reads
columns and calls the stages in order.

A staged batch carries every column of the scan and the selection of the last
stage. In a column read sparsely, a rejected row holds an unspecified value
that is valid for its type: zero, or an empty string. Operators above that
compute every row, as project kernels do, read such values and must not depend
on them. A column the column reader cannot read, such as a nested one, is read
with stage 0.

The price of keeping the ABI: one kernel per stage to compile, one pass over
the selection per stage, and buffers of the batch length for sparse columns.
Fusing removes the filter task, so a lane's scan and filter run on one worker
and the parallelism comes from the scan lanes alone.
