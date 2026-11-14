#include "pipeline.h"

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string_view>

// SQL text -> logical plan (binding, decorrelation) -> optimizer -> EXPLAIN printers.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    NQdb::NFuzz::RunStatements(
        std::string_view(reinterpret_cast<const char*>(data), size),
        [](const NQdb::NCompiler::TPreparedQuery& prepared, bool, size_t) {
            std::ostringstream output;
            NQdb::NCompiler::Explain(output, prepared.Plan, NQdb::NCompiler::EExplainMode::Both);
        });
    return 0;
}
