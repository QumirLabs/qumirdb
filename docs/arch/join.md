# Join architecture

This document describes hash matching, input scheduling, and ownership across
join modes.

## Overview

QDB implements equi-joins with a JIT-compiled symmetric hash join. The runtime
owns row storage, hash table headers, pair buffers, and output materialization.
Reusable Oz code implements the generic hash-table probe/insert logic, while
C++ generates query-specific key extraction, key hash/equality functions, and
optional residual predicates.

The important performance property is adaptive streaming after one input side
reaches EOF:

- while both inputs are still alive, both sides can be buffered and inserted;
- once one side reaches EOF, it becomes the build/materialized side;
- the remaining side switches to probe-only streaming;
- streamed rows are not inserted into their own hash table and are not retained
  in a `TRowStore`;
- output involving a streamed row is materialized immediately, while that
  stream batch is still alive.

This can avoid materializing the large side without a cardinality estimate.
Lowering can also choose build-first execution: inner joins use it for strong
asymmetry, while LEFT/RIGHT outer joins build their preserved side when it is
estimated no larger than the other input. A runtime filter may also require
completing its build input first. The build side finishes before the probe stream
starts; poor estimates can still affect work and memory.

## Supported join forms

Current production support:

| Join type | Status | Notes |
|---|---|---|
| `Inner` | Supported | Adaptive symmetric/probe-only hash join |
| `LeftSemi` | Supported | Left-preserving, emits left columns only |
| `LeftAnti` | Supported | Left-preserving, emits left columns only |
| `Left` | Supported | Preserved-left build with matched marks; symmetric fallback |
| `Right` | Supported | Preserved-right build with matched marks; symmetric fallback |
| `Full` | Supported | Symmetric path plus unmatched scans on both sides; matched marks with a residual |
| `RightSemi` / `RightAnti` | Not implemented in runtime | Logical enum exists, runtime guard rejects it |
| `LeftMark` | Kernel only | Left rows plus a three-valued `IN` marker; not planned yet |
| No equi-keys | Supported except `RightSemi` / `RightAnti` / `LeftMark` | Nested-loop `TCrossJoinProcessor` path |

Residual predicates are supported for every join type above except `LeftMark`,
which still rejects a residual. A `Left` or
`Right` join with a residual always takes the streaming path with its preserved
side as the build side. A build row is marked only after the residual accepted
a pair, so a row whose key matches were all rejected is emitted as unmatched.
The general outer path finds unmatched rows by key alone and cannot run a
residual, so a `Full` join with a residual marks the rows of both tables on its
symmetric path (see [Full outer](#full-outer)).

String, fixed-width, and nullable keys are supported, including composite keys.
Ordinary equality skips NULL keys; explicit null-safe keys use their declared
matching contract. Borrowed lookup keys and owned stored keys share compatible
hash/equality semantics.

## Output schema

`ComputeJoinOutputType` defines the logical output shape:

- `Inner`: all left columns followed by all right columns;
- `LeftSemi` / `LeftAnti`: left columns only;
- `RightSemi` / `RightAnti`: right columns only, typed but not executed;
- `Left`: left columns followed by nullable right columns;
- `Right`: nullable left columns followed by right columns;
- `Full`: nullable columns from both sides;
- `LeftMark`: not typed in plans yet; its kernel emits the left columns
  followed by the marker.

For joins that emit both sides, duplicate column names are an error. The
column-qualification pass normally prevents accidental collisions by rewriting
source fields to qualified names such as `lineitem.l_partkey`.

Column pruning runs before physical planning. Each join child keeps:

- its equi-key columns;
- columns referenced by the residual predicate on that side;
- columns required by the parent and belonging to that side.

The physical planner then computes runtime output types from the pruned child
schemas.

## Residual predicates

A residual predicate is a non-key predicate over the joined row, for example
`l1.l_suppkey != l2.l_suppkey` in TPC-H Q21.

Residual filtering happens inside the join kernel before a pair is emitted.
Generated code materializes only referenced columns and accepts the pair only
when the SQL predicate is TRUE; FALSE and UNKNOWN do not match.

Residual evaluation reads retained rows or the current transient probe batch.
Output involving the transient batch is materialized before that batch is
released; the join need not retain it just to evaluate the residual.

## Scheduling

`TInnerJoinProcessor` has three inner-join modes:

```text
Symmetric
StreamLeftAgainstRight
StreamRightAgainstLeft
```

In `Symmetric` mode, both sides are still live. A pulled batch is retained in
that side's `TRowStore`, processed by `ProcessLeft` or `ProcessRight`, and
inserted into that side's hash table.

The automatic input policy tracks retained row counts and recent batch sizes,
and prefers the side with fewer stored rows unless pulling one more batch of
that side would likely catch up to or exceed the other side. This matters for
TPC-H because Parquet batches can be large and join output batches can be
small. A naive batch round-robin can materialize most of a large fact table
before the small side reaches EOF. Row-balanced scheduling lets the small side
finish, then switches the large side to probe-only streaming.

When one side returns EOF:

- it becomes the complete build side;
- if the other side is still live, processing switches to probe-only mode;
- remaining batches from the live side call `ProbeLeftStream` or
  `ProbeRightStream`;
- the stream batch is materialized into owned output immediately and released.

Completion requires EOF on both build and stream inputs.

LEFT/RIGHT outer joins can build the preserved side to EOF and stream the
other input. Without a residual, lowering chooses this path when the preserved
side is estimated no larger than the other input, or when a runtime filter
requires building it first. A residual always requires this build side.

Build-first execution delays the other input, but retains and hashes only the
preserved side. The symmetric outer path avoids that initial wait and retains
both sides. Unlike inner execution, it cannot drop the remaining input after
one side ends, so the outer build-side rule needs no large asymmetry margin.
Unknown estimates or a larger preserved side keep the symmetric path unless a
residual or runtime filter requires streaming. FULL keeps the symmetric path.

## Execution by join type

### Inner

`Inner` uses the adaptive scheduler described above. Stored/stored matches go
through the generated output materializer. Stream/stored matches are materialized
immediately into `ReadyOutput_`.

### LeftSemi / LeftAnti without residual

The non-residual path is blocking but memory-efficient:

1. consume all left batches into `LeftRows_` and `LeftTable_`;
2. consume right batches through `InsertKeyOnly` into `RightTable_`, releasing
   each right batch immediately;
3. call `FinalizeAntiSemi(LeftTable_, RightTable_, pairs)`;
4. emit left rows from the resulting pair buffer.

The right side is never retained because only key existence is needed.

### LeftSemi / LeftAnti with residual

Residual semi/anti cannot use `InsertKeyOnly`, because the residual predicate
may reference right-side non-key columns. The blocking path (alongside supported build-first streaming paths):

1. consumes all left batches into `LeftRows_` and `LeftTable_`;
2. streams every right batch through `ProbeRightStream`;
3. the generated residual predicate reads left rows from `LeftRows_` and right
   rows from the transient stream batch;
4. filtered pairs contribute their left row ids to `MatchedLeftIds_`;
5. the executor emits each left row once based on membership.

The right side is not retained and is not inserted into `RightTable_`.

### Left / Right outer

The streaming path retains and hashes the preserved side, then probes it with
the other input. Each build row counts as matched only after a candidate pair
passes the residual predicate. Marks are per row, so duplicates sharing a key
can have different match outcomes. After the stream reaches EOF, the join emits
every unmarked build row once with NULLs for the absent side, including NULL-key
rows. Output pairs still refer to the retained build store and are drained in
batches before its storage is released.

The symmetric fallback retains both inputs, emits matches while inserting rows,
and finds unmatched preserved rows by checking the opposite table at EOF. It
supports joins without a residual; a residual requires the streaming path.

### Full outer

A FULL join preserves both inputs, so it cannot drop either one and always runs
symmetric: each row probes the table of the other side, then is inserted into
its own. Every pair is found once, by whichever of its two rows arrives later.
Without a residual, the unmatched rows of each table are those whose key the
other table lacks. With a residual, a key match no longer means a match, so the
probe marks each stored row of the other table whose pair the residual accepts,
and the arriving row is stored marked if it got any pair. At EOF both tables
emit their unmarked rows, NULL-key rows included, with NULLs for the other side.

### LeftMark

A mark join (Neumann, Leis, and Kemper, "The Complete Story of Joins (in
HyPer)", BTW 2017, section 3.3) emits every left row exactly once, followed by
a marker holding the SQL value of `key IN S`, where S is the multiset of right
keys: TRUE when S contains an equal key; otherwise NULL when S is non-empty and
the key is NULL or S contains NULL; otherwise FALSE. A NULL key against an empty
S is therefore FALSE. A NULL-aware `NOT IN` keeps the rows whose marker is
FALSE, which `LeftAnti` cannot express because it keeps every NULL key. The
marker column is `Nullable<bool>`, or `bool` when no key column on either side
is nullable and the marker therefore can never be NULL.

The join kernel is driven by a few operations. An update stores a batch of one
side in that side's hash table. A probe takes a batch of one side, looks its
rows up in the other side's table, which must already be complete, and emits
the results without storing the batch; the scheduler calls this streaming one
side against the other. Finalize runs after both inputs have ended.

For a mark join the right update stores only the distinct keys of S, as for
semi/anti, so duplicate right keys do not duplicate left rows. The marker of a
left row with no equal key depends on two facts about the whole of S: whether
S is empty, and whether S contains NULL. Both are known only after the last
right row, and the key set answers them: S is non-empty when the set's size is
not zero, and S contains NULL when the NULL key finds a slot, because ordinary
key equality groups all NULL keys into one slot. Left rows therefore get their
markers only once S is complete, in one of two ways that one run may combine:
stored by the left update and marked by finalize, or probed against the
finished set and marked immediately. The opposite direction, keeping the left
rows and passing S's rows by without storing them, is not supported: it would
need a match flag for every stored left key and the two facts about S kept
outside the key set.

The paper's hash mark join (section 5.6) takes that opposite direction: it
builds on the left input, streams the right one while marking left tuples, and
remembers whether the right input was empty or contained NULL. QDB builds S
instead, so the usually larger left input can be streamed. Each emitted pair
carries the left row id and, in place of a right row id, the marker code, so
the pair buffer and the generated materializer are shared with the other joins;
the materializer writes the marker as an ordinary bool column, bit-packed data
with a validity bitmap.

The kernel supports equi-keys without a residual predicate. A single key column
may be nullable on either side. A multi-column key must be non-nullable: with a
NULL component, the row comparison is unknown only against right rows that
match the other components, which the single NULL slot cannot tell. A mark
join without equi-keys is rejected: the nested-loop kernel has no marker. Plans
do not produce `LeftMark` yet: `ComputeJoinOutputType` rejects it because the
plan has no marker column name, and lowering rejects it because no join
processor drives it.

### Joins without equi-keys

A join without equi-keys, such as a cross join, `LEFT JOIN ... ON a < b`, or
the semi join of an uncorrelated `EXISTS`, is a nested-loop join in
`TCrossJoinProcessor`, separate from the hash join. It buffers the entire right
side, then streams left batches; the kernel compares each selected left row
with every selected right row and emits row-id pairs through the generated
output materializer. The join type fixes what the kernel emits:

- `Inner`: every pair; the residual is a separate filter over the product;
- `Left` / `Full`: every matching pair, and `(left, -1)` for a left row
  without a match;
- `LeftSemi` / `LeftAnti`: the left row once, when it has a match or has none;
  the scan of the right side stops at the first match;
- `Right` / `Full`: the kernel flags each matched right row, one byte per
  buffered right row, and after the last left batch emits `(-1, right)` for the
  unmatched ones.

For every type except `Inner` the residual decides which pairs match, so the
kernel evaluates it with the same generated predicate as the hash join.

In a parallel plan the right side is broadcast to every left lane. `Inner`,
`Left`, `LeftSemi` and `LeftAnti` keep the left partition count, because the
output for a left row depends on that row alone. `Right` and `Full` run on one
lane: a right row is unmatched only if no row of the whole left input matches
it. The browser runtime runs the same kernel for every type.

## Memory and lifetime invariants

The implementation relies on these invariants:

- each hash partition is one task, and a task runs on one worker at a time, so
  matched marks in its build table have one writer;
- outer unmatched pairs refer to the retained build rows until output is drained;
- a `TRowId` with non-negative batch index always refers to a retained
  `TRowStore` batch;
- a `TRowId` with batch index `-1` is valid only during one probe-only kernel
  call and only for residual evaluation/materialization from the stream batch;
- stream-side output is copied into owned output buffers before `Release` of
  the stream batch;
- `TRowStore::Data()` can be invalidated by vector growth, so C++ passes fresh
  store bases on every kernel call;
- `PairBuffer_.Count` is reset after each drain, but allocated capacity is
  reused;
- `ReadyOutput_` owns already-materialized output rowsets and is drained before
  pulling more input;
- the `TInnerJoinProcessor` destructor releases any queued `ReadyOutput_` batches if
  the consumer stops early.

## Current limitations

- Runtime kernels do not directly execute `RightSemi` / `RightAnti`.
- LEFT/RIGHT streaming requires a preserved build side selected before execution;
  outer joins do not switch adaptively when an input happens to end first.
- Intermediate rowsets remain explicit between operators; compiling kernels in
  one module does not imply cross-operator row-loop fusion.
