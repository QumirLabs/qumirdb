#include <qdb/sql/lexer.h>
#include <qdb/sql/parser.h>
#include <qdb/sql/printer.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <sstream>
#include <stdexcept>
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
    } catch (const std::out_of_range&) {
        // A bug (e.g. stoll on a long literal): qdb prints it as an error, but it is a finding.
        throw;
    } catch (const std::exception&) {
        // Deliberate errors of the query: the lexer throws on bad literals.
    }
    return 0;
}
