#pragma once

#include <cstddef>
#include <string>

namespace NQdb {

// Passes recurse once per tree level, so the depth of a query tree is bounded to keep
// hostile input from overflowing the stack (an ASan frame is the largest).
inline constexpr size_t MaxNesting = 256;

inline std::string NestingTooDeepMessage()
{
    return "query nesting is too deep (limit " + std::to_string(MaxNesting) + ")";
}

} // namespace NQdb
