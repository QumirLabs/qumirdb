#include <gtest/gtest.h>

#include <qdb/utils/config.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace NQdb::NUtils;

namespace {

TConfigSchema MakeSchema() {
    TConfigSchema schema;
    schema.Section("scheduler")
        .Enum("mode", { "single", "threaded" }, "threaded", "Execution mode.")
        .Int("workers", 4, "Worker threads.", 1, 256);
    schema.Section("scan")
        .Int("max_tasks", 0, "Scan task cap.")
        .Bool("prefetch", true, "Read ahead.")
        .Double("ratio", 0.5, "A ratio.")
        .String("dir", "", "A directory.");
    return schema;
}

std::string Failure(TConfig::TResult result) {
    EXPECT_FALSE(result.has_value());
    return result ? std::string() : result.error();
}

bool Contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

} // namespace

TEST(Config, DefaultsWithoutSources) {
    const auto schema = MakeSchema();
    TConfig config(schema);
    EXPECT_EQ(config.GetString("scheduler", "mode"), "threaded");
    EXPECT_EQ(config.GetInt("scheduler", "workers"), 4);
    EXPECT_TRUE(config.GetBool("scan", "prefetch"));
    EXPECT_EQ(config.GetDouble("scan", "ratio"), 0.5);
    EXPECT_EQ(config.GetString("scan", "dir"), "");
}

TEST(Config, ParsesSectionsCommentsAndWhitespace) {
    const auto schema = MakeSchema();
    TConfig config(schema);
    const auto result = config.LoadString(
        "# leading comment\r\n"
        "; another comment\n"
        "\n"
        "  [ scheduler ]  \n"
        "mode=single\n"
        "\tworkers   =   8   # trailing comment\n"
        "[scan]\n"
        "prefetch = false ; trailing comment\n"
        "dir = /tmp/a#b\n"
        "ratio = 1e-3\n");
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(config.GetString("scheduler", "mode"), "single");
    EXPECT_EQ(config.GetInt("scheduler", "workers"), 8);
    EXPECT_FALSE(config.GetBool("scan", "prefetch"));
    EXPECT_EQ(config.GetString("scan", "dir"), "/tmp/a#b");
    EXPECT_EQ(config.GetDouble("scan", "ratio"), 1e-3);
}

TEST(Config, LongLinesAreNotTruncated) {
    const auto schema = MakeSchema();
    TConfig config(schema);
    const std::string path(8000, 'x');
    ASSERT_TRUE(config.LoadString("[scan]\ndir = " + path + "\n").has_value());
    EXPECT_EQ(config.GetString("scan", "dir"), path);
}

TEST(Config, UnknownSectionListsAcceptedSections) {
    const auto schema = MakeSchema();
    TConfig config(schema);
    const auto error = Failure(config.LoadString("[sheduler]\n", "x.ini"));
    EXPECT_TRUE(Contains(error, "x.ini:1")) << error;
    EXPECT_TRUE(Contains(error, "sheduler")) << error;
    EXPECT_TRUE(Contains(error, "scheduler, scan")) << error;
}

TEST(Config, UnknownKeyListsAcceptedKeys) {
    const auto schema = MakeSchema();
    TConfig config(schema);
    const auto error = Failure(config.LoadString("[scan]\n\nprefech = true\n", "x.ini"));
    EXPECT_TRUE(Contains(error, "x.ini:3")) << error;
    EXPECT_TRUE(Contains(error, "prefech")) << error;
    EXPECT_TRUE(Contains(error, "max_tasks, prefetch, ratio, dir")) << error;
}

TEST(Config, MalformedLines) {
    const auto schema = MakeSchema();
    for (const char* text : {
             "[scan\n",
             "[scan]\nprefetch\n",
             "[scan]\n= true\n",
             "prefetch = true\n",
         })
    {
        TConfig config(schema);
        const auto error = Failure(config.LoadString(text, "x.ini"));
        EXPECT_TRUE(Contains(error, "x.ini:")) << error;
    }
}

TEST(Config, DuplicateKeyInOneSource) {
    const auto schema = MakeSchema();
    TConfig config(schema);
    const auto error = Failure(config.LoadString(
        "[scan]\nmax_tasks = 1\n[scheduler]\nworkers = 2\n[scan]\nmax_tasks = 2\n", "x.ini"));
    EXPECT_TRUE(Contains(error, "x.ini:6")) << error;
    EXPECT_TRUE(Contains(error, "duplicate key 'max_tasks'")) << error;
}

TEST(Config, WrongTypeAndRange) {
    const auto schema = MakeSchema();
    struct TCase {
        const char* Text;
        const char* Expected;
    };
    for (const auto& c : {
             TCase{ "[scan]\nprefetch = yes\n", "true or false" },
             TCase{ "[scan]\nmax_tasks = 1.5\n", "an integer >= 0" },
             TCase{ "[scan]\nmax_tasks = -1\n", "an integer >= 0" },
             TCase{ "[scan]\nmax_tasks =\n", "an integer >= 0" },
             TCase{ "[scan]\nratio = fast\n", "a number" },
             TCase{ "[scan]\nratio = nan\n", "a number" },
             TCase{ "[scheduler]\nworkers = 0\n", "an integer in 1..256" },
             TCase{ "[scheduler]\nworkers = 257\n", "an integer in 1..256" },
             TCase{ "[scheduler]\nworkers = 9999999999999999999999\n", "an integer in 1..256" },
         })
    {
        TConfig config(schema);
        const auto error = Failure(config.LoadString(c.Text, "x.ini"));
        EXPECT_TRUE(Contains(error, c.Expected)) << c.Text << " -> " << error;
        EXPECT_TRUE(Contains(error, "x.ini:2")) << error;
    }
}

TEST(Config, ValueOutsideEnumListsAcceptedValues) {
    const auto schema = MakeSchema();
    TConfig config(schema);
    const auto error = Failure(config.LoadString("[scheduler]\nmode = Single\n", "x.ini"));
    EXPECT_TRUE(Contains(error, "invalid value 'Single'")) << error;
    EXPECT_TRUE(Contains(error, "one of: single, threaded")) << error;
}

TEST(Config, FailedLoadChangesNothing) {
    const auto schema = MakeSchema();
    TConfig config(schema);
    EXPECT_FALSE(config.LoadString("[scheduler]\nworkers = 2\n[scan]\nprefetch = maybe\n").has_value());
    EXPECT_EQ(config.GetInt("scheduler", "workers"), 4);
}

TEST(Config, SourcesOverrideInOrder) {
    const auto schema = MakeSchema();
    TConfig config(schema);
    // defaults < default file < --config file < command-line overrides
    ASSERT_TRUE(config.LoadString("[scheduler]\nworkers = 2\n[scan]\nmax_tasks = 7\nratio = 1\n").has_value());
    EXPECT_EQ(config.GetInt("scheduler", "workers"), 2);
    ASSERT_TRUE(config.LoadString("[scheduler]\nworkers = 3\n[scan]\nmax_tasks = 9\n").has_value());
    EXPECT_EQ(config.GetInt("scheduler", "workers"), 3);
    ASSERT_TRUE(config.Override("scheduler:workers=5").has_value());
    EXPECT_EQ(config.GetInt("scheduler", "workers"), 5);
    EXPECT_EQ(config.GetInt("scan", "max_tasks"), 9);
    EXPECT_EQ(config.GetDouble("scan", "ratio"), 1.0);
    EXPECT_EQ(config.GetString("scheduler", "mode"), "threaded");
}

TEST(Config, OverrideErrors) {
    const auto schema = MakeSchema();
    TConfig config(schema);
    EXPECT_TRUE(Contains(Failure(config.Override("workers=5")), "section:key=value"));
    EXPECT_TRUE(Contains(Failure(config.Override("scan:prefetch")), "section:key=value"));
    EXPECT_TRUE(Contains(Failure(config.Override("nope:x=1")), "accepted sections: scheduler, scan"));
    EXPECT_TRUE(Contains(Failure(config.Override("scan:x=1")), "accepted keys: max_tasks, prefetch, ratio, dir"));
    EXPECT_TRUE(Contains(Failure(config.Override("scheduler:mode=fast")), "one of: single, threaded"));
    EXPECT_EQ(config.GetString("scheduler", "mode"), "threaded");
}

TEST(Config, OverrideValueMayContainColonAndEquals) {
    const auto schema = MakeSchema();
    TConfig config(schema);
    ASSERT_TRUE(config.Override("scan:dir=a:b=c").has_value());
    EXPECT_EQ(config.GetString("scan", "dir"), "a:b=c");
}

TEST(Config, LoadFile) {
    const auto schema = MakeSchema();
    const auto path = std::filesystem::temp_directory_path() / "qdb_test_config.ini";
    {
        std::ofstream out(path);
        out << "[scan]\nmax_tasks = 11\n";
    }
    TConfig config(schema);
    ASSERT_TRUE(config.LoadFile(path.string()).has_value());
    EXPECT_EQ(config.GetInt("scan", "max_tasks"), 11);
    std::filesystem::remove(path);

    const auto error = Failure(config.LoadFile(path.string()));
    EXPECT_TRUE(Contains(error, "cannot open")) << error;
}

TEST(Config, DumpLoadsBack) {
    const auto schema = MakeSchema();
    TConfig config(schema);
    ASSERT_TRUE(config.Override("scheduler:mode=single").has_value());
    ASSERT_TRUE(config.Override("scan:ratio=0.25").has_value());
    const auto dump = config.Dump();
    EXPECT_EQ(
        dump,
        "[scheduler]\nmode = single\nworkers = 4\n\n"
        "[scan]\nmax_tasks = 0\nprefetch = true\nratio = 0.25\ndir = \n");

    TConfig other(schema);
    ASSERT_TRUE(other.LoadString(dump).has_value());
    EXPECT_EQ(other.Dump(), dump);
}

TEST(Config, HelpListsEveryKey) {
    const auto schema = MakeSchema();
    EXPECT_EQ(
        schema.Help(),
        "[scheduler]\n"
        "  --scheduler:mode=<single|threaded>\n"
        "      Execution mode. (default: threaded)\n"
        "  --scheduler:workers=<int, 1..256>\n"
        "      Worker threads. (default: 4)\n"
        "[scan]\n"
        "  --scan:max_tasks=<int, min 0>\n"
        "      Scan task cap. (default: 0)\n"
        "  --scan:prefetch=<true|false>\n"
        "      Read ahead. (default: true)\n"
        "  --scan:ratio=<number>\n"
        "      A ratio. (default: 0.5)\n"
        "  --scan:dir=<string>\n"
        "      A directory. (default: empty)\n");
}

TEST(Config, SchemaMistakesThrow) {
    TConfigSchema schema;
    EXPECT_THROW(schema.Bool("a", true, ""), std::logic_error);
    schema.Section("s").Bool("a", true, "");
    EXPECT_THROW(schema.Bool("a", true, ""), std::logic_error);
    EXPECT_THROW(schema.Enum("e", { "x" }, "y", ""), std::logic_error);
    EXPECT_THROW(schema.Int("i", 5, "", 0, 4), std::logic_error);
}

TEST(Config, GettersRejectUnknownKeyAndWrongType) {
    const auto schema = MakeSchema();
    TConfig config(schema);
    EXPECT_THROW(config.GetInt("scan", "nope"), std::logic_error);
    EXPECT_THROW(config.GetBool("scan", "max_tasks"), std::logic_error);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
