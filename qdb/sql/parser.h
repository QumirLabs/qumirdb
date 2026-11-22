#pragma once

#include "lexer.h"
#include "ast.h"

#include <expected>
#include <vector>

#include <qumir/error.h>

namespace NQdb {
namespace NSql {

class TParser {
public:
    explicit TParser(NQumir::NAst::TTypeFactory& types)
        : Types_(types)
    { }

    std::expected<TSqlNodePtr, NQumir::TError> Parse(TTokenStream& stream);
    std::expected<std::vector<TSqlNodePtr>, NQumir::TError> ParseAll(
        TTokenStream& stream);

private:
    NQumir::NAst::TTypeFactory& Types_;
};

} // namespace NSql
} // namespace NQdb
