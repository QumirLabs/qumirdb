#include <qdb/utils/module_path.h>

#include <cstdint>
#include <optional>
#include <system_error>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace NQdb::NUtils {

namespace fs = std::filesystem;

namespace {

std::optional<fs::path> ExecutableDir() {
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) {
        return std::nullopt;
    }
#elif defined(__linux__)
    std::string buf = "/proc/self/exe";
#else
    return std::nullopt;
#endif
    std::error_code ec;
    // canonical() resolves the /usr/bin symlink into the versioned install tree.
    auto path = fs::canonical(buf, ec);
    if (ec) {
        return std::nullopt;
    }
    return path.parent_path();
}

// Installed, modules and kernel libraries share one directory; in the source tree
// they sit apart, so both roots are decided together.
struct TRoots {
    fs::path Modules;
    fs::path Kernels;
};

TRoots FindRoots() {
    std::error_code ec;
    if (auto dir = ExecutableDir()) {
        for (const auto& candidate : {
            *dir / ".." / "modules",
            *dir / ".." / "share" / "qumirdb" / "modules",
            // A self-contained bundle, such as the fuzz targets of ClusterFuzzLite.
            *dir / "modules",
        }) {
            if (fs::is_directory(candidate, ec)) {
                auto normalized = candidate.lexically_normal();
                return {normalized, normalized};
            }
        }
    }
    return {fs::path(QDB_SOURCE_DIR) / "modules", fs::path(QDB_SOURCE_DIR) / "kernel"};
}

const TRoots& Roots() {
    static const TRoots roots = FindRoots();
    return roots;
}

} // namespace

const fs::path& ModulesDir() {
    return Roots().Modules;
}

const fs::path& KernelDir() {
    return Roots().Kernels;
}

std::string ModuleFile(std::string_view name) {
    return (ModulesDir() / name).string();
}

std::string KernelFile(std::string_view kind, std::string_view name) {
    return (KernelDir() / kind / name).string();
}

} // namespace NQdb::NUtils
