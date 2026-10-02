#include <qdb/exec/filter_exec.h>

#include <memory>
#include <utility>
#include <vector>

namespace NQdb {

namespace {

// Owns the input rowset and a private selection buffer so the batch survives
// reuse of the kernel state. Columns is filled only when the output is narrowed.
struct TFilterViewData {
    TRowSet Input = {};
    std::vector<TColumn> Columns;
    std::vector<uint8_t> Selection;
};

void DestroyFilterViewRowSet(TRowSet* rowSet) {
    auto* data = static_cast<TFilterViewData*>(rowSet->Private);
    Release(&data->Input);
    delete data;
}

// Predicate over the full input; emit an owning view of keptIndices (keepAll =
// every column, zero-copy).
TUnaryStreamProcess MakeFilterViewProcess(
    TKernelCompiler::TFilterDispatch dispatch,
    std::vector<int32_t> keptIndices,
    bool keepAll)
{
    return [dispatch = std::move(dispatch),
            keptIndices = std::move(keptIndices),
            keepAll](TRowSet& rowSet, TUnaryStreamingKernelState&) {
        auto data = std::make_unique<TFilterViewData>();
        data->Selection.resize(static_cast<size_t>(rowSet.RowCount));

        // Predicate on a copy; leave the input intact until it succeeds.
        TRowSet filtered = rowSet;
        filtered.Selection = data->Selection.data();
        dispatch(filtered);
        if (rowSet.Selection) {
            for (int64_t row = 0; row < rowSet.RowCount; ++row) {
                if (rowSet.Selection[row] == 0) {
                    data->Selection[static_cast<size_t>(row)] = 0;
                }
            }
        }

        TColumn* columns = rowSet.Columns;
        int64_t columnCount = rowSet.ColumnCount;
        if (!keepAll) {
            data->Columns.reserve(keptIndices.size());
            for (int32_t idx : keptIndices) {
                data->Columns.push_back(rowSet.Columns[idx]);
            }
            columns = data->Columns.data();
            columnCount = static_cast<int64_t>(data->Columns.size());
        }
        // Take ownership last (default dtor won't Release on throw).
        data->Input = rowSet;

        auto* view = data.release();
        rowSet = {
            .Columns = columns,
            .ColumnCount = columnCount,
            .RowCount = rowSet.RowCount,
            .Selection = view->Selection.data(),
            .Hash = rowSet.Hash,
            .Destroy = DestroyFilterViewRowSet,
            .Private = view,
            .RefCount = 1,
        };
    };
}

} // namespace

TUnaryStreamProcess MakeFilterProcess(
    TKernelCompiler::TFilterDispatch dispatch)
{
    return MakeFilterViewProcess(std::move(dispatch), {}, /*keepAll=*/true);
}

TUnaryStreamProcess MakeFilterSelectProcess(
    TKernelCompiler::TFilterDispatch dispatch,
    std::vector<int32_t> keptIndices)
{
    return MakeFilterViewProcess(
        std::move(dispatch), std::move(keptIndices), /*keepAll=*/false);
}

} // namespace NQdb
