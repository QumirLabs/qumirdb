#include <gtest/gtest.h>

#include <qdb/kernel/lib.h>
#include <qdb/modules/qumirdb.h>
#include <qdb/modules/qumirdb_types.h>
#include <qdb/modules/qumirdb_runtime.h>

#include "qumirdb_source_module.h"

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/runner/runner_llvm.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace {

std::unique_ptr<NQumir::TLLVMRunner> CompileStringOperation(
    const std::string& entryName,
    void*& entry)
{
    auto library = NQdb::NKernel::ParseFunctionLibrary(
        NQdb::NKernel::ReadAggregationKernel("string_ops.oz"));
    if (!library) {
        ADD_FAILURE() << library.error().ToString();
        return {};
    }

    NQumir::TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    options.AllowOverloads = true;
    NQdb::NTest::ConfigureQumirDbSourceModule(options);
    auto runner = std::make_unique<NQumir::TLLVMRunner>(options);
    auto program = std::make_shared<NQumir::NAst::TBlockExpr>(
        NQumir::TLocation{}, std::move(*library));
    NQdb::NTest::AddQumirDbUse(program);
    std::string error;
    entry = runner->CompileKernelAst(program, entryName, &error);
    EXPECT_NE(entry, nullptr) << error;
    return runner;
}

NQdb::TStringView View(const std::string& value) {
    return {
        .Data = reinterpret_cast<uint8_t*>(const_cast<char*>(value.data())),
        .Size = static_cast<int64_t>(value.size()),
    };
}

TEST(QumirDbStringOps, LikeFastPathsMatchByteSemantics) {
    auto sv = [](std::string_view s) {
        return qdb_string_view{
            reinterpret_cast<const uint8_t*>(s.data()),
            static_cast<int64_t>(s.size())};
    };
    EXPECT_EQ(qdb_like_equals(sv("google"), sv("google")), 1);
    EXPECT_EQ(qdb_like_equals(sv("google"), sv("googl")), 0);
    EXPECT_EQ(qdb_like_equals(sv(""), sv("")), 1);

    EXPECT_EQ(qdb_like_prefix(sv("google.com"), sv("google")), 1);
    EXPECT_EQ(qdb_like_prefix(sv("a.google"), sv("google")), 0);
    EXPECT_EQ(qdb_like_prefix(sv("x"), sv("")), 1);
    EXPECT_EQ(qdb_like_prefix(sv(""), sv("g")), 0);

    EXPECT_EQ(qdb_like_suffix(sv("a.google"), sv("google")), 1);
    EXPECT_EQ(qdb_like_suffix(sv("google.com"), sv("google")), 0);
    EXPECT_EQ(qdb_like_suffix(sv("x"), sv("")), 1);

    EXPECT_EQ(qdb_like_contains(sv("a.google.com"), sv("google")), 1);
    EXPECT_EQ(qdb_like_contains(sv("a.bing.com"), sv("google")), 0);
    EXPECT_EQ(qdb_like_contains(sv("goog"), sv("google")), 0);
    EXPECT_EQ(qdb_like_contains(sv("anything"), sv("")), 1);
}

// The oz kernel must agree with the C++ reference around every block boundary.
TEST(QumirDbStringOps, LikeContainsKernelMatchesReference) {
    auto library = NQdb::NKernel::ParseFunctionLibrary(
        "(block (fun like_contains_entry"
        " ((var str StringView) (var lit StringView)) -> i64"
        " (block (return (call qdb_like_contains str lit)))))");
    ASSERT_TRUE(library) << library.error().ToString();
    NQumir::TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    options.AllowOverloads = true;
    NQdb::NTest::ConfigureQumirDbSourceModule(options);
    NQumir::TLLVMRunner runner(options);
    auto program = std::make_shared<NQumir::NAst::TBlockExpr>(
        NQumir::TLocation{}, std::move(*library));
    NQdb::NTest::AddQumirDbUse(program);
    std::string error;
    void* entry = runner.CompileKernelAst(program, "like_contains_entry", &error);
    ASSERT_NE(entry, nullptr) << error;
    auto contains = reinterpret_cast<int64_t(*)(
        NQdb::TStringView, NQdb::TStringView)>(entry);
    auto reference = [](const std::string& str, const std::string& lit) {
        return qdb_like_contains(
            {reinterpret_cast<const uint8_t*>(str.data()),
                static_cast<int64_t>(str.size())},
            {reinterpret_cast<const uint8_t*>(lit.data()),
                static_cast<int64_t>(lit.size())});
    };

    for (const std::string lit : {"", "g", "ge", "google", "g-seventeen-bytes"}) {
        for (size_t size = 0; size <= 70; ++size) {
            // Decoys share the first and last literal byte with real matches.
            std::string miss(size, 'x');
            for (size_t i = 0; i < size; i += 3) {
                miss[i] = i % 2 ? 'e' : 'g';
            }
            EXPECT_EQ(contains(View(miss), View(lit)), reference(miss, lit))
                << "lit=" << lit << " size=" << size;
            for (size_t at = 0; at + lit.size() <= size; ++at) {
                std::string hit = miss;
                hit.replace(at, lit.size(), lit);
                EXPECT_EQ(contains(View(hit), View(lit)), 1)
                    << "lit=" << lit << " size=" << size << " at=" << at;
            }
        }
    }
}

// Hash value is an implementation detail, so check properties, not magic numbers.
TEST(QumirDbStringOps, HashIsDeterministicAndContentSensitive) {
    auto hash = qdb_string_hash_bytes;
    auto hashOf = [&](const std::string& s) {
        return hash(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    };

    const std::string empty;
    const std::string ascii = "foobar";
    const std::string prefix = "abc";
    const std::string longer = "abcd";
    const std::string utf8 = "\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87";
    const std::string embeddedZero("a\0b", 3);
    const std::string embeddedZeroDifferent("a\0c", 3);
    // Long enough to exercise the >16-byte XXH3 path.
    const std::string url = "https://example.com/path/to/resource?query=1";

    EXPECT_EQ(hashOf(empty), hashOf(""));
    EXPECT_EQ(hashOf(ascii), hashOf(ascii));

    EXPECT_NE(hashOf(empty), hashOf(ascii));
    EXPECT_NE(hashOf(prefix), hashOf(longer)) << "length must affect the hash";
    EXPECT_NE(hashOf(embeddedZero), hashOf(embeddedZeroDifferent))
        << "bytes after an embedded zero must not be dropped";
    EXPECT_NE(hashOf(prefix), hashOf(utf8));
    EXPECT_NE(hashOf(url), hashOf(url.substr(0, url.size() - 1)))
        << "the tail byte must count";

    // Same bytes, different alignment.
    for (int padding = 0; padding < 8; ++padding) {
        const std::string padded = std::string(padding, 'x') + url;
        auto padHash = hash(
            reinterpret_cast<const uint8_t*>(padded.data()) + padding,
            static_cast<int64_t>(url.size()));
        EXPECT_EQ(padHash, hashOf(url)) << "padding=" << padding;
    }
}

TEST(QumirDbStringOps, EqualityUsesBytesAndLength) {
    void* entry = nullptr;
    auto runner = CompileStringOperation("qdb_string_equal_bytes", entry);
    ASSERT_NE(entry, nullptr);
    auto equal = reinterpret_cast<bool(*)(
        const uint8_t*, int64_t, const uint8_t*, int64_t)>(entry);

    const std::string prefix = "abc";
    const std::string longer = "abcd";
    const std::string same = "abc";
    const std::string embeddedZero("a\0b", 3);
    const std::string embeddedZeroDifferent("a\0c", 3);
    EXPECT_TRUE(equal(
        reinterpret_cast<const uint8_t*>(prefix.data()), prefix.size(),
        reinterpret_cast<const uint8_t*>(same.data()), same.size()));
    EXPECT_FALSE(equal(
        reinterpret_cast<const uint8_t*>(prefix.data()), prefix.size(),
        reinterpret_cast<const uint8_t*>(longer.data()), longer.size()));
    EXPECT_FALSE(equal(
        reinterpret_cast<const uint8_t*>(embeddedZero.data()), embeddedZero.size(),
        reinterpret_cast<const uint8_t*>(embeddedZeroDifferent.data()),
        embeddedZeroDifferent.size()));
}

TEST(QumirDbStringOps, CompareIsUnsignedByteLexicographic) {
    void* entry = nullptr;
    auto runner = CompileStringOperation("qdb_string_compare_bytes", entry);
    ASSERT_NE(entry, nullptr);
    auto compare = reinterpret_cast<int64_t(*)(
        const uint8_t*, int64_t, const uint8_t*, int64_t)>(entry);

    const std::string prefix = "abc";
    const std::string longer = "abcd";
    const std::string highByte("\xC3\xA9", 2);
    const std::string ascii = "z";
    EXPECT_EQ(compare(
        reinterpret_cast<const uint8_t*>(prefix.data()), prefix.size(),
        reinterpret_cast<const uint8_t*>(prefix.data()), prefix.size()), 0);
    EXPECT_LT(compare(
        reinterpret_cast<const uint8_t*>(prefix.data()), prefix.size(),
        reinterpret_cast<const uint8_t*>(longer.data()), longer.size()), 0);
    EXPECT_GT(compare(
        reinterpret_cast<const uint8_t*>(longer.data()), longer.size(),
        reinterpret_cast<const uint8_t*>(prefix.data()), prefix.size()), 0);
    EXPECT_GT(compare(
        reinterpret_cast<const uint8_t*>(highByte.data()), highByte.size(),
        reinterpret_cast<const uint8_t*>(ascii.data()), ascii.size()), 0);
}

TEST(QumirDbStringOps, CopyPreservesEmbeddedZero) {
    void* entry = nullptr;
    auto runner = CompileStringOperation("qdb_string_copy_bytes", entry);
    ASSERT_NE(entry, nullptr);
    auto copy = reinterpret_cast<int64_t(*)(uint8_t*, const uint8_t*, int64_t)>(entry);

    const std::array<uint8_t, 4> source = {'a', 0, 'b', 0xff};
    std::array<uint8_t, 4> destination{};
    EXPECT_EQ(copy(destination.data(), source.data(), source.size()), source.size());
    EXPECT_EQ(destination, source);
}

TEST(QumirDbStringOps, ViewAndOwnedOverloadsShareContract) {
    void* hashViewEntry = nullptr;
    auto hashViewRunner = CompileStringOperation(
        "aggregation_string_hash_view", hashViewEntry);
    ASSERT_NE(hashViewEntry, nullptr);

    const std::string value("same\0bytes", 10);
    auto view = View(value);
    NQdb::TOwnedString owned{.Data = view.Data, .Size = view.Size};
    auto hashView = reinterpret_cast<int64_t(*)(NQdb::TStringView)>(hashViewEntry);
    const int64_t viewHash = hashView(view);

    void* hashOwnedEntry = nullptr;
    auto hashOwnedRunner = CompileStringOperation(
        "aggregation_string_hash_owned", hashOwnedEntry);
    ASSERT_NE(hashOwnedEntry, nullptr);
    auto hashOwned = reinterpret_cast<int64_t(*)(NQdb::TOwnedString)>(hashOwnedEntry);
    EXPECT_EQ(hashOwned(owned), viewHash);

    void* equalEntry = nullptr;
    auto equalRunner = CompileStringOperation(
        "aggregation_string_equal_view_owned", equalEntry);
    ASSERT_NE(equalEntry, nullptr);
    auto equal = reinterpret_cast<bool(*)(NQdb::TStringView, NQdb::TOwnedString)>(
        equalEntry);
    EXPECT_TRUE(equal(view, owned));
}

TEST(QumirDbStringOps, SqlLikeMatchesSqlWildcards) {
    void* likeEntry = (void*)qdb_string_view_sql_like;
    ASSERT_NE(likeEntry, nullptr);

    auto like = reinterpret_cast<int64_t(*)(NQdb::TStringView, NQdb::TStringView)>(
        likeEntry);

    const std::string value("same\0bytes", 10);
    auto view = View(value);

    EXPECT_EQ(like(view, View("same%")), 1);
    EXPECT_EQ(like(view, View("same_bytes")), 1);
    EXPECT_EQ(like(view, View("%bytes")), 1);
    EXPECT_EQ(like(view, View("%bytez")), 0);
    EXPECT_EQ(like(view, View("same%xxx")), 0);

    EXPECT_EQ(like(view, View("same")), 0);
    EXPECT_EQ(like(view, View("same_bytes_")), 0);
    EXPECT_EQ(like(view, View("same%xxx")), 0);
}

TEST(QumirDbStringOps, SqlLikeHandlesEmptyAndPercentOnlyPatterns) {
    void* likeEntry = (void*)qdb_string_view_sql_like;
    ASSERT_NE(likeEntry, nullptr);

    auto like = reinterpret_cast<int64_t(*)(NQdb::TStringView, NQdb::TStringView)>(
        likeEntry);

    EXPECT_EQ(like(View(""), View("")), 1);
    EXPECT_EQ(like(View("abc"), View("%")), 1);
    EXPECT_EQ(like(View(""), View("%")), 1);
    EXPECT_EQ(like(View("abc"), View("%%")), 1);
    EXPECT_EQ(like(View("abc"), View("")), 0);
}

TEST(QumirDbStringOps, SqlLikeBacktracksOverPercent) {
    void* likeEntry = (void*)qdb_string_view_sql_like;
    ASSERT_NE(likeEntry, nullptr);

    auto like = reinterpret_cast<int64_t(*)(NQdb::TStringView, NQdb::TStringView)>(
        likeEntry);

    EXPECT_EQ(like(View("abbbc"), View("a%c")), 1);
    EXPECT_EQ(like(View("abbbc"), View("a%b%c")), 1);
    EXPECT_EQ(like(View("abbbc"), View("a%bc")), 1);
    EXPECT_EQ(like(View("abbbc"), View("a%d")), 0);
    EXPECT_EQ(like(View("mississippi"), View("m%iss%ppi")), 1);
}

TEST(DateYear, KnownDates) {
    // 1970-01-01 = day 0
    EXPECT_EQ(qdb_date_year(0), 1970);
    // 1995-01-01 = 9131 days (used in TPC-H Q7/Q8/Q9)
    EXPECT_EQ(qdb_date_year(9131), 1995);
    // 1996-12-31 = 9861 days
    EXPECT_EQ(qdb_date_year(9861), 1996);
    // 1998-12-01 (approximate Q1 upper bound area)
    EXPECT_EQ(qdb_date_year(10471), 1998);
    // 2000-01-01 (leap year boundary)
    EXPECT_EQ(qdb_date_year(10957), 2000);
    // Negative: 1969-12-31 = -1
    EXPECT_EQ(qdb_date_year(-1), 1969);
}

} // namespace

int main(int argc, char** argv) {
    NQumir::NCodeGen::TLLVMInitializer initializer;
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
