#pragma once

// Test-only helper: run a logical plan through the scheduler and expose the
// buffered output via a pull `Next()` interface, so tests that previously drove
// the deleted `IRuntimeNode`/`TPhysicalPlanner` pull engine keep working with a
// one-line change (`RunPlan(root[, settings])` instead of building a planner).

#include <qdb/io/io.h>
#include <qdb/scheduler/plan_lowerer.h>
#include <qdb/scheduler/settings.h>

#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace NQdb {

class TTestRuntime {
public:
    TTestRuntime(std::vector<TRowSet> rowSets, NQumir::NAst::TTypePtr outputType)
        : RowSets_(std::move(rowSets))
        , OutputType_(std::move(outputType))
    {}

    ~TTestRuntime() {
        for (size_t i = Pos_; i < RowSets_.size(); ++i) {
            if (RowSets_[i].RefCount) {
                Release(&RowSets_[i]);
            }
        }
    }

    bool Next(TRowSet& rowSet) {
        if (Pos_ >= RowSets_.size()) {
            return false;
        }
        rowSet = RowSets_[Pos_];
        RowSets_[Pos_] = {};
        ++Pos_;
        return true;
    }

    NQumir::NAst::TTypePtr OutputType() const { return OutputType_; }

private:
    std::vector<TRowSet> RowSets_;
    size_t Pos_ = 0;
    NQumir::NAst::TTypePtr OutputType_;
};

namespace NTestDetail {

// Buffers each output rowset (single-owner copy, as the old buffered pipeline
// did) so the test can inspect them after the scheduler finishes.
class TCollectingSink : public ISink {
public:
    void Write(const TRowSet& rowSet) override {
        auto* retained = const_cast<TRowSet*>(&rowSet);
        Retain(retained);
        auto copy = *retained;
        copy.RefCount = 1;
        RowSets.push_back(copy);
    }

    std::vector<TRowSet> RowSets;
};

} // namespace NTestDetail

inline std::unique_ptr<TTestRuntime> RunPlan(
    const TOperatorPtr& root,
    NScheduler::TSettings settings = {},
    std::shared_ptr<const TExternalCatalogSnapshot> externalCatalog = nullptr,
    std::shared_ptr<IRuntimeFilterBindingFactory> filterBindings = nullptr)
{
    auto lowered = NScheduler::LowerPlanToGraph(
        root, settings, nullptr, std::move(externalCatalog),
        std::move(filterBindings));
    auto outputType = lowered.OutputType;

    NTestDetail::TCollectingSink sink;
    std::string error;
    if (!NScheduler::RunPlanIntoSink(
            std::move(lowered), sink, settings, nullptr, &error)) {
        throw std::runtime_error(error);
    }
    return std::make_unique<TTestRuntime>(
        std::move(sink.RowSets), std::move(outputType));
}

} // namespace NQdb
