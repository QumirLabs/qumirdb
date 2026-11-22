#pragma once

#include <qdb/io/io.h>
#include <qdb/io/value_type.h>

#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace NQdb {

// PostgreSQL-style aligned table output.
// Buffers all rows; call Flush() (or destroy) to print.
class TConsoleSink : public ISink {
public:
    TConsoleSink(const TSchema& schema, std::ostream& out);

    void Write(const TRowSet& rowSet) override;
    void Flush() override;

private:
    const TSchema& Schema_;
    // By column; nullopt prints "?".
    std::vector<std::optional<TValueType>> Values_;
    std::ostream& Out_;
    std::vector<std::vector<std::string>> Rows_;
    std::vector<size_t> Widths_;
};

// CSV output with configurable separator.
class TCsvSink : public ISink {
public:
    TCsvSink(const TSchema& schema, std::ostream& out, char separator = ',', bool noEscape = false);

    void Write(const TRowSet& rowSet) override;

private:
    const TSchema& Schema_;
    // By column; nullopt prints "?".
    std::vector<std::optional<TValueType>> Values_;
    std::ostream& Out_;
    char Separator_;
    bool NoEscape_ = false;
    bool HeaderPrinted_ = false;
};

class TNullSink : public ISink {
public:
    explicit TNullSink(const TSchema& schema) : Schema_(schema) {}

    void Write(const TRowSet& rowSet) override;

private:
    const TSchema& Schema_;
    int64_t Rows_ = 0;
};

} // namespace NQdb
