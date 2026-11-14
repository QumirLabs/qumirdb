#include <qdb/sql/lexer.h>
#include <qdb/sql/parser.h>
#include <qdb/sql/printer.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <sstream>
#include <string>

// SQL text -> lexer -> parser -> AST printer.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::istringstream input(std::string(reinterpret_cast<const char*>(data), size));
    try {
        NQdb::NSql::TTokenStream tokens(input);
        NQdb::NSql::TParser parser;
        auto statements = parser.ParseAll(tokens);
        if (statements) {
            for (const auto& statement : *statements) {
                NQdb::NSql::PrintAst(statement);
            }
        }
    } catch (const std::exception&) {
        // qdb reports these as errors of the query: the lexer throws on bad literals.
    }
    return 0;
}
