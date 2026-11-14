#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace NQdb::NUtils {

// Directory holding the `.oz` modules: the versioned install tree next to the
// binary, the prefix install, modules/ beside the binary, or the source tree of
// this build.
const std::filesystem::path& ModulesDir();

// Root of the `.oz` kernel libraries; their aggregation/join/sort subdirectories
// live next to the modules once installed.
const std::filesystem::path& KernelDir();

std::string ModuleFile(std::string_view name);

std::string KernelFile(std::string_view kind, std::string_view name);

} // namespace NQdb::NUtils
