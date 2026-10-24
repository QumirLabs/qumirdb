#pragma once

#include <qdb/utils/regex.h>

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

struct qdb_string_view;

// Chunked bump arena for strings produced during kernel evaluation (e.g. `||`
// concatenation via qdb_string_concat). It never moves bytes it has already handed
// out - new blocks are appended, existing blocks stay put - so every StringView it
// returns stays valid for the whole batch, until the executor copies the bytes into
// the owned output column, or until a row-at-a-time kernel calls Reset. One instance
// lives per kernel invocation (owned by the executor) and frees all blocks on
// destruction.
class TStringArena {
public:
    char* Alloc(int64_t size);
    // Invalidates every view handed out so far and keeps one block for reuse.
    void Reset();
    NQdb::NUtils::TRegexContext& RegexContext() { return RegexContext_; }

private:
    NQdb::NUtils::TRegexContext RegexContext_;
    std::vector<std::vector<char>> Blocks_;
    int64_t Used_ = 0; // bytes used in the last block
};

class TCompiledRegex {
public:
    TCompiledRegex(std::string_view pattern, std::string_view replacement);
    ~TCompiledRegex();

    TCompiledRegex(const TCompiledRegex&) = delete;
    TCompiledRegex& operator=(const TCompiledRegex&) = delete;

    qdb_string_view Replace(TStringArena& arena, qdb_string_view input) const;

private:
    struct TImpl;
    std::unique_ptr<TImpl> Impl_;
};

extern "C" {

// Backing allocator for qdb-generated kernels (e.g. the Robin Hood hash
// table used by the Aggregation operator). Thin wrappers around malloc
// family so they can be registered as external functions in the QumirDb
// module and resolved by the LLVM JIT via the process symbol table.
void* qdb_alloc(int64_t size);
void qdb_free(void* ptr);

int64_t qdb_filter_string_compare(
    const uint8_t* left, int64_t leftSize,
    const uint8_t* right, int64_t rightSize);

// Same algorithm as arrow::internal::ComputeStringHash<0> (arrow/util/hashing.h):
// inline multiply-mix for <=16 bytes, XXH3 with a fixed secret above that.
int64_t qdb_string_hash_bytes(const uint8_t* data, int64_t size);

struct qdb_string_view {
    const uint8_t* Data;
    int64_t Size;
};

struct qdb_bin_int {
    uint64_t Lo;
    uint64_t Hi;
};

int64_t qdb_string_view_sql_like(qdb_string_view str, qdb_string_view pattern);

int64_t qdb_like_equals(qdb_string_view str, qdb_string_view lit);
int64_t qdb_like_prefix(qdb_string_view str, qdb_string_view lit);
int64_t qdb_like_suffix(qdb_string_view str, qdb_string_view lit);
int64_t qdb_like_contains(qdb_string_view str, qdb_string_view lit);

// TODO: needs qumir changes - temporary cast so string literals reach nullable
// StringView operators without str_from_lit (not linked in JIT kernels).
qdb_string_view qdb_lit_to_sv(const char* lit);

qdb_string_view qdb_substring(qdb_string_view str, int32_t start, int32_t length);

// SQL `left || right` string concatenation. `arena` is a TStringArena* (opaque here);
// the concatenated bytes are bump-allocated from it so the returned view outlives the
// kernel row loop. Null propagation is handled by the caller (ExpandNullable), so this
// is unconditional: it always concatenates both operands.
qdb_string_view qdb_string_concat(void* arena, qdb_string_view left, qdb_string_view right);

// TStringArena::Reset for a kernel that evaluates one row at a time, such as a
// join residual, once the strings of the current row are dead.
void qdb_string_arena_reset(void* arena);

// regex is compiled once while the query is lowered. arena owns bytes produced
// by replacements until the current kernel invocation has consumed them.
qdb_string_view qdb_regexp_replace(
    void* arena, const TCompiledRegex* regex, qdb_string_view input);

// Returns the Gregorian year for a date given as days since 1970-01-01.
int32_t qdb_date_year(int32_t days);

// SQL DATE '<yyyy-mm-dd>' literal -> days since 1970-01-01.
int32_t qdb_sql_date(qdb_string_view date);

// SQL INTERVAL '<amount>' <unit> -> days (year/month approximate: 365/30).
int32_t qdb_sql_interval(qdb_string_view amount, qdb_string_view unit);

// SQL ROUND(x, digits) -> round half away from zero.
double qdb_round(double value, int32_t digits);

int32_t qdb_abs_i32(int32_t value);
int64_t qdb_abs_i64(int64_t value);
double qdb_fabs(double value);

qdb_bin_int qdb_decimal_from_i64(int64_t value, int64_t scale);
qdb_bin_int qdb_decimal_from_f64(double value, int64_t scale);
qdb_bin_int qdb_decimal_scale_up(qdb_bin_int value, int64_t delta);
qdb_bin_int qdb_decimal_add(qdb_bin_int left, qdb_bin_int right);
qdb_bin_int qdb_decimal_sub(qdb_bin_int left, qdb_bin_int right);
qdb_bin_int qdb_decimal_neg(qdb_bin_int value);
qdb_bin_int qdb_decimal_mul_i64(qdb_bin_int left, int64_t right);
qdb_bin_int qdb_decimal_div_i64(qdb_bin_int left, int64_t right);
qdb_bin_int qdb_decimal_div(qdb_bin_int left, qdb_bin_int right);
bool qdb_decimal_eq(qdb_bin_int left, qdb_bin_int right);
bool qdb_decimal_ne(qdb_bin_int left, qdb_bin_int right);
bool qdb_decimal_lt(qdb_bin_int left, qdb_bin_int right);
bool qdb_decimal_le(qdb_bin_int left, qdb_bin_int right);
bool qdb_decimal_gt(qdb_bin_int left, qdb_bin_int right);
bool qdb_decimal_ge(qdb_bin_int left, qdb_bin_int right);

} // extern "C"
