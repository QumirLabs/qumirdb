#pragma once

#include <qdb/io/parquet/source.h>
#include <qdb/plan/pipeline.h>
#include <qdb/scheduler/settings.h>
#include <qdb/utils/config.h>

namespace NQdb {

// Everything an application tunes in the engine, from the optimizer to the
// Arrow allocator. The applications fill the parts they do not tune from
// their own context (diagnostics, annotation) and pass them on.
struct TEngineSettings {
    NScheduler::TSettings Scheduler;
    TPlanPassOptions Optimizer;
    // Arrow memory pools are process singletons: apply it once at startup.
    EArrowMemoryPool ArrowMemoryPool = EArrowMemoryPool::Auto;
};

// The one table of settings the config accepts: every section and key with its
// type, default and description, taken from a default-constructed
// TEngineSettings. A new setting is one line in engine_settings.cpp.
const NUtils::TConfigSchema& EngineConfigSchema();

// Applies the keys that files or overrides set onto `settings`; the others leave
// it as it is, so a caller can set its own defaults first.
void ApplyEngineConfig(const NUtils::TConfig& config, TEngineSettings& settings);

} // namespace NQdb
