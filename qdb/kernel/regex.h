#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

class TCompiledRegex;

namespace NQdb::NKernel {

struct TRegexSpec {
    std::string Pattern;
    std::string Replacement;

    void Fields(this auto& self, auto& v)
    {
        v(self.Pattern);
        v(self.Replacement);
    }
};

// Query-local immutable-at-execution registry. Register is called serially
// during lowering; generated kernels receive Handles()[id], never the registry.
class TRegexRegistry {
public:
    size_t Register(std::string pattern, std::string replacement);

    const std::vector<TRegexSpec>& Specs() const;
    void** Handles();

private:
    std::vector<TRegexSpec> Specs_;
    std::vector<std::shared_ptr<TCompiledRegex>> Compiled_;
    std::vector<void*> Handles_;
};

} // namespace NQdb::NKernel
