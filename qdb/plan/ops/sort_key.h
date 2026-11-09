#pragma once

#include <string>

namespace NQdb {

enum class ESortDirection {
    Asc,
    Desc,
};

enum class ESortNulls {
    Default,
    First,
    Last,
};

// NULL sorts as larger than every value (PostgreSQL), so the default is NULLS
// LAST for ASC and NULLS FIRST for DESC. An explicit NULLS FIRST/LAST wins.
// Every consumer resolves a key through this function.
inline ESortNulls EffectiveSortNulls(ESortDirection direction, ESortNulls nulls) {
    if (nulls != ESortNulls::Default) {
        return nulls;
    }
    return direction == ESortDirection::Desc ? ESortNulls::First : ESortNulls::Last;
}

struct TSortKey {
    std::string Column;
    ESortDirection Direction = ESortDirection::Asc;
    ESortNulls Nulls = ESortNulls::Default;

    void Fields(this auto& self, auto& v)
    {
        v(self.Column);
        v(self.Direction);
        v(self.Nulls);
    }
};

} // namespace NQdb
