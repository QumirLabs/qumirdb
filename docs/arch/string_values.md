# String values in kernels

This document explains borrowed column values and owned keys across kernel
and operator lifetimes.

## Column and kernel representations

QDB exposes Arrow strings as a variable-width `TColumn`:

```text
Data        -> contiguous UTF-8 bytes
Offsets     -> i32[] or i64[]
OffsetWidth -> 4 or 8
Mask        -> optional validity bitmap
```

Qumir `string` is a different, managed runtime object. It is represented by a
pointer to NUL-terminated data with a refcounted header. Casting `TColumn.Data`
to `<ptr string>` is therefore invalid. QDB instead materializes a borrowed
string view from the column's byte storage and offsets.

Aggregation has an additional lifetime problem: input batches are released
immediately after update, while group keys must remain valid until finalize.

## Value types

QDB kernels must not use Qumir `TStringType` or the Qumir managed `string`
runtime at all. `TStringType` may remain the logical schema marker received
from Arrow, but kernel lowering replaces it with QDB-owned named struct types.

Use two distinct named POD structs with the same physical shape:

```text
StringView  = { Data: <ptr u8>, Size: i64 }
OwnedString = { Data: <ptr u8>, Size: i64 }
```

`StringView` is borrowed from a `TRowSet` and is valid only while that rowset is
alive. `OwnedString` points to immutable bytes owned by the aggregation table
or, later, by the query allocator.

These are ordinary structs in the Qumir AST. They must not trigger string
retain/release, NUL-terminated operations, Unicode indexing, or any other
managed-string lowering.

`OwnedString` is a semantic name, not per-value RAII. Copying or swapping the
two-field struct does not transfer or duplicate ownership. This property is
required because hash-table growth moves keys and the same logical key is
also referenced by dense output storage.

Lengths are byte lengths, not Unicode symbol counts. Hashing, equality,
ordering, Parquet offsets, and CSV output all operate on UTF-8 bytes.

## Column materialization

One generated helper must materialize logical row values for both filter and
aggregation. For a string column and row `i` it reads `Offsets[i]` and
`Offsets[i + 1]`, handling both offset widths, and constructs a `StringView`.

The Parquet source currently normalizes `Data` to the first selected value but
leaves offsets based at `Offsets[0]`. Materialization must therefore use:

```text
start = offsets[i]     - offsets[0]
end   = offsets[i + 1] - offsets[0]
view  = { Data + start, end - start }
```

This logic must not be duplicated between filter and aggregation.

## Why lookup and storage keys differ

Using one owned key for both lookup and storage would force a string copy
before every lookup, including duplicates. Generated aggregation code therefore
derives two representations:

```text
LookupKey  = fixed fields + StringView leaves
StoredKey  = fixed fields + OwnedString leaves
```

For a scalar string these are the corresponding scalar named types. For a
composite key, QDB generates parallel named structs with identical logical
field order but different string leaf types.

The table API is conceptually:

```text
lookup(StoredKey[], LookupKey) -> dense slot or -1
insert(StoredKey[], StoredKey, dense slot)
rehash(StoredKey[] -> StoredKey[])
```

The input remains borrowed during lookup. Only an absent key is cloned into
owned storage.

The distinction is operational, not only nominal:

| Property | `LookupKey` | `StoredKey` |
| --- | --- | --- |
| String leaf | `StringView` | `OwnedString` |
| Byte owner | Input `TRowSet` | Hash table |
| Lifetime | One update call/batch | Until table destruction |
| Construction | Materialize without allocation | Clone only after lookup miss |
| Probe argument | Yes | Existing resident key |
| Table storage | No | Yes |
| Rehash | Never retained | Handle copied without copying bytes |

For fixed-width leaves both representations use the same concrete type. For
example, a logical `{i64,string}` key becomes `{i64,StringView}` for lookup and
`{i64,OwnedString}` for storage. Both representations must hash identically
and support direct `StoredKey` versus `LookupKey` equality.

The dual-key contract applies to every key type. It does not introduce a
string-only branch in the hash table:

```text
fixed-width: LookupKey == StoredKey
string-bearing: LookupKey != StoredKey
```

Every descriptor generates the same ownership helpers. For a key without
owned leaves, `key_owned_bytes` returns zero and `key_clone_owned` is an
identity operation that ignores its byte buffer. Generic update code can
therefore always execute `lookup -> allocate an arena slice -> clone -> insert`;
the allocation step is skipped when the requested size is zero.

## Owned byte storage

Each new group receives owned bytes for its string key leaves. Slices come
from a table-owned chunked arena, amortizing allocation across many groups.
Only the newest chunk accepts new slices; unused tails trade some memory for
constant-time bump allocation. Oversized values receive a suitable chunk.

Rehash copies `OwnedString` handles without cloning or freeing their bytes.
Allocation follows a borrowed lookup miss. An insertion failure rewinds the
latest slice, and table destruction releases registered arena chunks once.
Exact chunk sizes and registry layout are implementation details in the wiki.

## String output

String key output uses Arrow-compatible variable-width column storage:

```text
Data        -> copied UTF-8 bytes
Offsets     -> i64[row_count + 1]
OffsetWidth -> 8
```

Use a two-pass finalize:

1. Measure total output bytes for every string group-key column.
2. C++ allocates output `Data` and `Offsets` buffers.
3. Generated finalize writes offsets and copies bytes from dense `StoredKey`
   values.
4. The table may then be destroyed; output no longer references its arena.

Finalization produces column descriptors carrying data, offsets, and validity.
The output owner remains independent of the table arena.

## Filtering

Filtering consumes the same generated `StringView` values, but it does
not need owned strings. The filter expression boundary needs explicit
StringView operations for at least:

- equality and inequality;
- lexical ordering;
- optional prefix/contains helpers where the expression permits.

Column-to-column comparisons can be fully `StringView` based. String literals
in query expressions must also be lowered by QDB into static byte storage plus
a `StringView` struct. They must not first become Qumir managed strings.

Parsing and annotation of the final kernel AST must see both column bindings
and literals as `StringView`. Otherwise Qumir can select managed-string
operators and insert retain/release behavior inappropriate for Arrow memory.

Kernel specialization rewrites logical string bindings and literals to
`StringView`, then resolves and annotates the complete generated kernel.
Reusing a managed-string predicate without this representation change would
not be sufficient.

## Null semantics

Null representation is not string-specific. Every `TColumn` uses the same
`Mask + MaskBitOffset` validity bitmap, regardless of its value type.
`StringView` describes only the bytes of a valid string value.

The column format determines whether a row value is null, but operator code
still determines what null means:

- `GROUP BY`: null keys participate in grouping; nulls in the same key position
  compare equal for grouping, and their payload bytes must not be read;
- `WHERE`: a null/unknown predicate does not select the row; compound
  nullable predicates preserve three-valued logic;
- `count(*)` counts rows, `count(arg)` skips null arguments, and `sum/min/max`
  skip null inputs and need a defined empty-group result;
- output columns carry nulls through their own `TColumn.Mask`.

Validity should therefore be added once to the common column materializer and
generated key/reducer semantics, for fixed-width and string values together.
It must not be encoded inside `StringView` or implemented as a separate string
rule.

## Required invariants

- A `StringView` never outlives its input `TRowSet`.
- A null value is represented by column validity, not by a special
  `StringView{Data,Size}` value.
- Stored keys never reference input batch memory.
- `OwnedString` bytes remain immutable until table destruction.
- Equal lookup/stored keys always produce the same hash.
- Probe-table rebuild and rehash never allocate or free string bytes.
- Final output owns its bytes and does not reference the table arena.
- String comparisons use byte length and do not depend on NUL termination.
- No generated QDB kernel value has Qumir `string` type.
