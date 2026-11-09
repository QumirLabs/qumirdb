#pragma once

namespace NQdb {

enum class EJoinType {
    Inner,
    Left,       // LEFT OUTER
    Right,      // RIGHT OUTER
    Full,       // FULL OUTER
    LeftSemi,
    RightSemi,
    LeftAnti,
    RightAnti,
    // Every left row once, followed by a marker with the SQL value of
    // `left keys IN right keys` (TRUE / FALSE / NULL). Only the join kernel
    // supports it so far; plans do not produce it.
    LeftMark,
};

} // namespace NQdb
