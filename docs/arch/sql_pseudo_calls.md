# SQL expression pseudo-calls

QDB uses call-shaped AST nodes for SQL semantics that do not map one-to-one to
a runtime function. They preserve information through parsing and logical plan
rewrites, then `qdb/kernel/annotate_type.cpp` expands them once operand types and
nullability are known.

Keep this list updated when adding another call name handled specially by
`InferType`, `ExpandNullable`, or kernel generation.

## Expansion-only calls

| Call | Produced by | Expansion |
|---|---|---|
| `qdb_in_list(lhs, item...)` | SQL scalar `IN (...)` | Binds `lhs` once, hoists its nullable guard, and builds equality/OR expressions over its plain value. Per-item three-valued logic remains only for nullable items. The call never reaches Qumir name resolution. |
| `strcat(a, b)` | SQL `a || b` | Becomes `qdb_string_concat(__arena__, a, b)`. `__arena__` is the project kernel's hidden scratch-arena parameter. `strcat` itself is not a runtime symbol. |
| `upper(str)`, `lower(str)` | SQL function syntax | Become `qdb_string_upper(__arena__, str)` and `qdb_string_lower(__arena__, str)`, oz-lang functions in `qumirdb.oz` that write the result into the scratch arena. They apply DuckDB's simple (one code point to one code point) Unicode case mapping in Basic Latin, Latin-1 Supplement, Latin Extended-A, Latin Extended Additional, the Greek alphabet U+0386-U+03CE, Cyrillic and Cyrillic Supplement, and keep every other character, for example Latin Extended-B, Greek Extended, Armenian, Georgian and fullwidth Latin. Bytes that are not UTF-8 are copied unchanged. |
| `regexp_replace(str, pattern, replacement)` | SQL function syntax | Constant `pattern` and `replacement` are registered once per query, then the call becomes `qdb_regexp_replace(__arena__, __regexes__[id], str)`. The current three-argument form replaces only the first match; flags are not supported yet. |
| `qdb_string_view_sql_like(str, pattern)` | SQL `str LIKE pattern` | When `pattern` is a constant with no `_`, the expander picks a direct match from the shape of its `%`: none is `qdb_like_equals`, a trailing `%` is `qdb_like_prefix`, a leading `%` is `qdb_like_suffix`, both are `qdb_like_contains`. These run a `memcmp` or `memmem` on the string bytes. Any other pattern keeps the general matcher `qdb_string_view_sql_like`. |
| `coalesce(a, ...)` | SQL function syntax | Becomes a typed `if` chain selecting the first valid argument. |

`qdb_in_list` unwraps `Nullable[T]` internally only after testing `Valid`. There
is intentionally no standalone `qdb_unwrap_nullable` call: an unguarded unwrap
would make it easy to discard SQL NULL semantics.

The native executor compiles regular expressions with PCRE2, while the browser
executor uses JavaScript's ECMAScript `RegExp`. The engine does not currently
validate a common regex subset, so engine-specific constructs may compile or
behave differently between native and browser execution. Portable queries must
use constructs with equivalent semantics in both dialects.

## Semantic bridge calls

These names have implementations or overloads in `qdb/modules/qumirdb.oz`, but
QDB typing also recognizes them specially because their SQL null behavior is
not ordinary null propagation.

| Call | Contract |
|---|---|
| `qdb_sql_null()` | Represents a bare, initially untyped SQL `NULL`. Its enclosing expression supplies the eventual `Nullable[T]` value type. |
| `qdb_is_null(x)` | SQL `IS NULL`: `!x.Valid` for `Nullable[T]`, and constant `false` for non-nullable `T`. |
| `qdb_is_true(x)` | Predicate boundary used by `WHERE`, `HAVING`, join residuals, and `CASE WHEN`. It accepts a row only when the SQL value is true; `NULL` is not true. |

## Kernel placeholders

`__arena__` and `__regexes__` are identifier placeholders rather than calls.
Filter and project kernel generation declares them as the opaque per-invocation
string scratch arena and the query-local compiled-regex handle table. Computed
sort, grouping, aggregate, and window expressions are materialized by a project
before their key kernels. A regex call left directly in a join or low-level
aggregate kernel is rejected explicitly instead of reaching the linker.
