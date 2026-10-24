# Parquet decoding

QDB decodes Parquet pages with an oz-lang kernel that writes values straight
into the `TColumn` buffers of a batch. Arrow's reader remains the fallback for
files and columns the kernel does not support, and still serves late
materialization lookups.

## Host and kernel boundary

A scan task owns the I/O and the memory; the decoder kernel owns the format.

```text
host (C++)                              decoder kernel (oz-lang)
read column chunk bytes  ───────────▶   walk page headers (Thrift compact)
                                         decompress page  ◀── qdb_parquet_decompress
                                         dictionary page → entry table
                                         definition levels → validity bitmap
own and recycle batch buffers ◀───────   RLE/bit-packed indices, PLAIN values
                                         → TColumn data, offsets, mask
```

The host reads the bytes of one column chunk for a row group and describes how
its values map to a `TColumn`: a fixed width copied verbatim, a boolean bitmap,
or byte arrays as int32 offsets plus data. The kernel parses page headers,
loads dictionary pages, decodes definition levels, and decodes values of
consecutive rows into the batch. It calls back into the host for exactly one
thing: decompressing a page into a buffer it provides.

Page headers are parsed in the kernel rather than by the host because they are
part of the format: a host that only supplies chunk bytes and a codec leaves no
Parquet logic to reimplement in another runtime. A header costs a few hundred
instructions per page of thousands of values, so its placement does not matter
for speed. Native execution binds the decompression primitive to libzstd and
snappy; the browser would bind it to a JavaScript or wasm codec, and the same
kernel compiles to wasm64.

The decoder is a fixed oz library rather than generated code. Lowering adds it
to a query that scans Parquet; its functions come from the persistent JIT
cache, and only its three entry points are compiled with the query. The
functions those entry points call are marked `noinline`, which keeps that
compilation from importing the decoder behind them.

## Dictionaries and strings

Dictionary encoding stores each distinct value once per column chunk and refers
to it by a bit-packed index (the Parquet format's RLE/bit-packing hybrid). The
kernel decodes a dictionary page once per chunk. For fixed-width types the
decoded page is the dictionary; for byte arrays the kernel builds one entry per
value, its offset and length within the page.

A `TColumn` string column needs the bytes of consecutive rows to be adjacent,
so a batch cannot point into the dictionary: each row's value is copied into
the batch's data buffer. When the longest dictionary value fits 16 bytes, the
copy is a single 16-byte load and store regardless of the actual length; longer
values copy in 16-byte pieces. Buffers carry enough padding for these
over-reads and over-writes. The per-batch cost is therefore proportional to the
batch's rows and bytes, never to the dictionary, and no per-value builder or
validity bookkeeping takes part.

Indices are unpacked eight at a time by a function that takes the bit width as
a compile-time generic parameter: each width gets its own instance in which
every shift and byte offset is a constant.

## Batch size

A scan's decoded columns are written by the decoder and read by the next
kernel of the pipeline immediately afterwards. When the batch is small enough
for those columns to stay in the core's private cache, the second pass costs
cache hits instead of memory traffic. This is the vector-size argument of
MonetDB/X100: all vectors of a pipeline should fit the cache, yet be large
enough to amortize per-call overhead (Boncz, Zukowski, Nes, *MonetDB/X100:
Hyper-Pipelining Query Execution*, CIDR 2005, §5.1.1). DuckDB decodes Parquet
into its fixed 2048-row vectors.

QDB's batches, however, also cross task boundaries and feed operators whose
policies are tuned for large batches: the symmetric hash join schedules its
inputs by stored rows and recent batch sizes, the runtime-filter probe
vectorizes only large batches, and the hash shuffle forwards a large batch as
a selection view but copies small ones together. Lowering therefore sizes the
batches of each scan from the plan. A scan whose pipeline - the source and the
filters and projections after it - ends in a join input produces batches of
2^18 rows, the size of the Arrow path. Any other scan produces 2048-row
batches, which pays off where a scan feeds a filter and an aggregate.

## Fallback

The kernel path is chosen per scan source during lowering. It requires a flat
schema, physical and logical types whose `TColumn` representation is a verbatim
copy (32- and 64-bit integers and dates, doubles, booleans, UTF-8 strings),
PLAIN or dictionary encodings, and uncompressed, Snappy or Zstandard chunks.
Any other column or chunk of the source makes the whole source use Arrow;
`--verbose` reports the reason. `--parquet-decoder arrow` selects Arrow for
every source.

References: Apache Parquet format specification, data pages and encodings
(github.com/apache/parquet-format).
