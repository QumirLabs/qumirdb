# Late Materialization

Late materialization can make a wide query faster when the query has a small
`LIMIT`.

## Basic Idea

A normal Parquet scan reads every output column before the limit. With this
rule, QDB first reads only the filter and sort columns. It also creates a row
locator for each row. After the limit, QDB uses the locators to fetch the other
columns only for the result rows.

```sql
SELECT *
FROM hits
WHERE "URL" LIKE '%google%'
ORDER BY "EventTime"
LIMIT 10;
```

The scheduler graph has this general form:

```text
narrow scan -> filter -> top sort or limit -> broadcast -> lookup tasks -> merge
```

The scan can use several lanes. QDB gathers their locator rows into one stream.
If there is more than one lookup task, a small unary bridge sends this stream
to a `TBroadcastConnection`. QDB skips the bridge and the broadcast when it
uses one lookup task.

QDB splits the output columns into adjacent ranges. It uses the number of
columns, not their size in bytes. In threaded mode with two or more workers,
QDB can create up to four lookup tasks per worker. Any worker can take the next
small task from the ready queue, so a costly column has less effect. One worker
uses one lookup task. Single-threaded mode also uses one task.

All lookup tasks run in the scheduler pool. The final merge keeps the column
data buffers in place, so it does not copy their data.

## Row Locator

The row locator is one `u64` value. The high 32 bits store the Parquet row
group id. The low 32 bits store the row offset in that group.

The name `__row_id__` is reserved. A Parquet file cannot have a column with
this name. A SQL query cannot use it as an output name.

## Planner Rule

The rule is on by default. It changes a plan only when all these checks pass:

- The plan has one Parquet source.
- Between the source and the limit, filters keep the same row identity.
- The final output has direct source columns, with no computed expressions.
- The plan has a known positive limit. The default maximum is 100 rows.
- The eager scan estimate is at least two times the late estimate.

The cost model adds the narrow scan bytes and the lookup bytes. It gets
compressed and uncompressed sizes from Parquet metadata. It uses 1 MiB as an
estimated page size. It also computes an upper bound from full row-group
chunks. The lower value becomes the lookup estimate. This estimate is not an
exact count of bytes from disk.

## Parquet Lookup

The source resolves column names and types once for each lookup task. Each task
gets a read-only physical row reader. The readers share the `FileReader` that
the source has already opened. QDB does not open the file again.

For supported flat primitive and string columns, the reader first tries the
page path. This path reads only pages that contain requested rows. Some column
types and page layouts cannot use this path. In that case, the reader loads the
affected row groups and then selects the rows. Both paths keep column order and
duplicate column names.

If the page path starts, only `Invalid` and `NotImplemented` can cause a move
to the row-group path. An I/O error or a memory error stops the query.

The shared `FileReader` depends on three current rules:

- The input is an Arrow `ReadableFile`.
- Arrow pre-buffer is off.
- The lookup code does not use page-index or bloom-filter readers.

Code that changes one of these rules must also check shared access again.

## Logical Plan

The logical operator stores its input, locator name, and column map. It does
not store a live source pointer. After an S-expression parse, `SourceFactory`
provides the source. The plan pipeline checks that the operator has one lookup
source and then turns row locators on.


Browser plan export disables the rule because that runtime has no source
lookup task.
