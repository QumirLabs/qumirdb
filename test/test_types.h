#pragma once

#include <qdb/kernel/annotate_type.h>
#include <qdb/kernel/query_modules.h>

#include <qumir/parser/type_factory.h>

#include <memory>

namespace NQdb {

// The module base of a unit test binary, built once as a process builds its own.
inline const std::shared_ptr<const NKernel::TModuleBase>& TestModuleBase() {
    static const auto base = std::make_shared<const NKernel::TModuleBase>();
    return base;
}

// The type factory of a unit test binary, as in Qumir's tests: types are interned and
// immutable, so tests that share it cannot interfere. A test that compiles a plan
// compiles it on this factory, since its tables and expressions are built in it.
inline const std::shared_ptr<NQumir::NAst::TTypeFactory>& SharedTestTypes() {
    static const auto types = NKernel::MakeQueryTypes(*TestModuleBase());
    return types;
}

inline NQumir::NAst::TTypeFactory& TestTypes() {
    return *SharedTestTypes();
}

// A new annotation context over the test factory, without an external catalog.
inline NKernel::TAnnotationContext TestAnnotation() {
    return NKernel::MakeAnnotationContext(TestTypes(), TestModuleBase(), nullptr);
}

} // namespace NQdb
