#pragma once

#include <qdb/kernel/query_modules.h>

#include <qumir/parser/type_factory.h>

#include <memory>

namespace NQdb {

// The type factory of a unit test binary, as in Qumir's tests: types are interned and
// immutable, so tests that share it cannot interfere. A test that compiles a plan
// compiles it on this factory, since its tables and expressions are built in it.
inline const std::shared_ptr<NQumir::NAst::TTypeFactory>& SharedTestTypes() {
    static const auto types = NKernel::MakeQueryTypes();
    return types;
}

inline NQumir::NAst::TTypeFactory& TestTypes() {
    return *SharedTestTypes();
}

} // namespace NQdb
