#include "lexer.h"

#include <cctype>
#include <stdexcept>
#include <string>
#include <unordered_set>

#include <qumir/parser/operator.h>

namespace NQdb {
namespace NSql {

using namespace NQumir;
using namespace NQumir::NAst;

namespace {

static const std::unordered_set<std::string> TwoCharOperators = {
    "<=", ">=", "<>", "!=", "&&", "||", "::"
};

static const std::unordered_set<int> TwoCharOperatorFirstChars = {
    '<', '>', '!', '|', '&', ':'
};

static const std::unordered_set<int> SingleCharOperators = {
    '(', ')', ',', ';', '=', '+', '-', '*', '/', '%', '<', '>', '.'
};

static const std::unordered_set<std::string> Keywords = {
    "SELECT",
    "FROM",
    "WHERE",
    "GROUP",
    "BY",
    "HAVING",

    "ORDER",
    "LIMIT",
    "OFFSET",

    "JOIN",
    "INNER",
    "LEFT",
    "RIGHT",
    "FULL",
    "CROSS",
    "NATURAL",
    "OUTER",
    "SEMI",
    "ON",
    "USING",

    "AS",

    "WITH",
    "RECURSIVE",

    "NULLS",
    "FIRST",
    "LAST",

    "UNION",
    "EXCEPT",
    "INTERSECT",
    "ALL",
    "DISTINCT",

    "CASE",
    "WHEN",
    "THEN",
    "ELSE",
    "END",

    "AND",
    "OR",
    "NOT",

    "IS",
    "NULL",

    "IN",
    "BETWEEN",
    "LIKE",
    "EXISTS",

    "ASC",
    "DESC",

    "CAST",

    "EXTRACT",
    "SUBSTRING",
    "FOR",

    "TRUE",
    "FALSE",

    "DATE",
    "INTERVAL",

    "ROLLUP",
    "CUBE",
    "GROUPING",
    "SETS",

    "PARTITION",
};

} // namespace

TTokenStream::TTokenStream(std::istream& in)
    : ITokenStream(in)
{
}

void TTokenStream::Read() {
    auto take = [&]() {
        char ch = 0;
        In.get(ch);
        AdvanceLocation(CurrentLocation, ch);
        return ch;
    };

    auto unget = [](auto& stream, int ch) {
        if constexpr (requires { stream.unget(ch); }) {
            stream.unget(ch);
        } else {
            stream.unget();
        }
    };

    auto emitOperator = [&](TOperator op, const std::string& rawValue, TLocation location) {
        Tokens.emplace_back(TToken {
            .Value = {.i64 = (int64_t)op},
            .RawValue = rawValue,
            .Type = TToken::Operator,
            .Location = location,
        });
    };

    auto readQuoted = [&](char quote) {
        std::string value;
        std::string rawValue;
        bool escaped = false;

        while (In.peek() != -1) {
            char ch = take();
            rawValue += ch;
            if (escaped) {
                value += Unescape(ch);
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == quote) {
                rawValue.pop_back();
                return std::make_pair(value, rawValue);
            } else {
                value += ch;
            }
        }

        throw std::runtime_error("unterminated literal");
    };

    auto readSqlQuotedIdentifier = [&]() {
        std::string value;
        std::string rawValue;

        while (In.peek() != -1) {
            char ch = take();

            if (ch == '"') {
                if (In.peek() == '"') {
                    take();
                    value += '"';
                    rawValue += "\"\"";
                    continue;
                }
                return std::make_pair(value, rawValue);
            }

            value += ch;
            rawValue += ch;
        }

        throw std::runtime_error("unterminated quoted identifier");
    };

    auto readSqlString = [&]() {
        std::string value;
        std::string rawValue;

        while (In.peek() != -1) {
            char ch = take();

            if (ch == '\'') {
                if (In.peek() == '\'') {
                    take();
                    value += '\'';
                    rawValue += "''";
                    continue;
                }
                return std::make_pair(value, rawValue);
            }

            value += ch;
            rawValue += ch;
        }

        throw std::runtime_error("unterminated string literal");
    };

    auto readDollarQuotedString = [&]() {
        take(); // $
        take(); // $
        std::string value;
        while (In.peek() != -1) {
            char ch = take();
            if (ch == '$' && In.peek() == '$') {
                take();
                return value;
            }
            value += ch;
        }
        throw std::runtime_error("unterminated dollar-quoted string");
    };

    auto readNumber = [&](TLocation location) {
        std::string rawValue;
        bool isFloat = false;

        if (In.peek() == '.') {
            isFloat = true;
            rawValue += take();
        }

        while (In.peek() != -1 && std::isdigit(In.peek())) {
            rawValue += take();
        }

        if (In.peek() == '.') {
            isFloat = true;
            rawValue += take();
            while (In.peek() != -1 && std::isdigit(In.peek())) {
                rawValue += take();
            }
        }

        if (In.peek() == 'e' || In.peek() == 'E') {
            isFloat = true;
            rawValue += take();
            if (In.peek() == '+' || In.peek() == '-') {
                rawValue += take();
            }
            if (In.peek() == -1 || !std::isdigit(In.peek())) {
                throw std::runtime_error("expected digit in exponent at " + CurrentLocation.ToString());
            }
            while (In.peek() != -1 && std::isdigit(In.peek())) {
                rawValue += take();
            }
        }

        if (isFloat) {
            Tokens.emplace_back(TToken {
                .Value = {.f64 = std::stod(rawValue)},
                .RawValue = rawValue,
                .Type = TToken::Float,
                .Location = location,
            });
        } else {
            Tokens.emplace_back(TToken {
                .Value = {.i64 = std::stoll(rawValue)},
                .RawValue = rawValue,
                .Type = TToken::Integer,
                .Location = location,
            });
        }
    };

    auto isTwoCharOperator = [&](int next) -> bool {
        if (!TwoCharOperatorFirstChars.count(next)) {
            return false;
        }
        In.get();
        auto second = In.peek();
        unget(In, next);
        std::string opStr;
        opStr += (char)next;
        opStr += (char)second;
        return TwoCharOperators.count(opStr) > 0;
    };

    auto isNumberStart = [&](int next) -> bool {
        if (std::isdigit(next)) {
            return true;
        }
        if (next == '.') {
            In.get();
            auto second = In.peek();
            unget(In, next);
            return std::isdigit(second);
        }
        return false;
    };

    auto skipLineComment = [&]() {
        take(); // -
        take(); // -
        while (In.peek() != -1 && In.peek() != '\n') {
            take();
        }
    };

    auto skipBlockComment = [&]() {
        take(); // /
        take(); // *

        while (In.peek() != -1) {
            char ch = take();
            if (ch == '*' && In.peek() == '/') {
                take();
                return;
            }
        }

        throw std::runtime_error("unterminated block comment at " + CurrentLocation.ToString());
    };

    auto isIdentStart = [](int ch) {
        return std::isalpha(ch) || ch == '_';
    };

    auto isIdentChar = [](int ch) {
        return std::isalnum(ch) || ch == '_';
    };

    while (Tokens.empty() && In.peek() != -1) {
        auto next = In.peek();
        if (std::isspace(next)) {
            take();
            continue;
        }

        TLocation tokenLocation = CurrentLocation;

        if (next == '-') {
            In.get();
            auto second = In.peek();
            unget(In, next);

            if (second == '-') {
                skipLineComment();
                continue;
            }
        }

        if (next == '/') {
            In.get();
            auto second = In.peek();
            unget(In, next);

            if (second == '*') {
                skipBlockComment();
                continue;
            }
        }

        if (next == '$') {
            auto value = readDollarQuotedString();
            Tokens.emplace_back(TToken {
                .Name = value,
                .RawValue = value,
                .Type = TToken::String,
                .Location = tokenLocation,
            });
        } else if (isTwoCharOperator(next)) {
            std::string name;
            name += take();
            name += take();
            emitOperator(TOperator(name), name, tokenLocation);
        } else if (isNumberStart(next)) {
            readNumber(tokenLocation);
        } else if (SingleCharOperators.count(next)) {
            auto ch = take();
            emitOperator(TOperator((uint64_t)ch), std::string(1, ch), tokenLocation);
        } else if (next == '"') {
            take();
            auto [value, rawValue] = readSqlQuotedIdentifier();
            Tokens.emplace_back(TToken {
                .Name = value,
                .RawValue = rawValue,
                .Type = TToken::Identifier,
                .Location = tokenLocation,
            });
        } else if (next == '\'') {
            take();
            auto [value, rawValue] = readSqlString();
            Tokens.emplace_back(TToken {
                .Name = value,
                .RawValue = rawValue,
                .Type = TToken::String,
                .Location = tokenLocation,
            });
        } else if (next == '`') {
            take();
            auto [value, rawValue] = readQuoted('`');
            Tokens.emplace_back(TToken {
                .Name = value,
                .RawValue = rawValue,
                .Type = TToken::Identifier,
                .Location = tokenLocation,
            });
        } else {
            if (!isIdentStart(next)) {
                throw std::runtime_error("unexpected character at " + CurrentLocation.ToString());
            }

            std::string name;
            while (In.peek() != -1 && isIdentChar(In.peek())) {
                name += take();
            }

            std::string upperName;
            for (char c : name) {
                upperName += std::toupper(c);
            }
            if (Keywords.count(upperName)) {
                Tokens.emplace_back(TToken {
                    .Name = upperName,
                    .RawValue = name,
                    .Type = TToken::Keyword,
                    .Location = tokenLocation,
                });
            } else {
                // SQL folds unquoted identifiers case-insensitively; parquet columns and
                // .oz function names are lowercase, so fold to lowercase. Quoted
                // identifiers (handled above) keep their case.
                std::string lowerName;
                for (char c : name) {
                    lowerName += std::tolower(c);
                }
                Tokens.emplace_back(TToken {
                    .Name = lowerName,
                    .RawValue = name,
                    .Type = TToken::Identifier,
                    .Location = tokenLocation,
                });
            }
        }
    }
}

} // namespace NSql
} // namespace NQdb
