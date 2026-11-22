#pragma once

#include <qdb/io/io.h>
#include <qdb/plan/ops/operator.h>

#include <cstdint>
#include <string>

namespace NQdb {

struct TLateMaterializationSettings {
    bool Enabled = true;
    uint64_t MaxOutputRows = 100;
    double MinSavingsFactor = 2.0;
};

struct TLateMaterializationDiagnostics {
    bool Considered = false;
    bool Applied = false;
    std::string Reason;
    uint64_t Limit = 0;
    size_t EarlyColumnCount = 0;
    size_t FetchColumnCount = 0;
    TLateMaterializationCost Cost;
};

void BindLateMaterializationSources(NQumir::NAst::TTypeFactory& types, const TOperatorPtr& root);

TOperatorPtr ApplyLateMaterialization(
    NQumir::NAst::TTypeFactory& types,
    const TOperatorPtr& root,
    const TLateMaterializationSettings& settings = {},
    TLateMaterializationDiagnostics* diagnostics = nullptr);

} // namespace NQdb
