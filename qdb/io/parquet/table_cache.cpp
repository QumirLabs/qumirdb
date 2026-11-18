#include "table_cache.h"

#include <cerrno>
#include <filesystem>

#include <sys/stat.h>

#include "source.h"

namespace NQdb {

TTableCache::TFileIdentity TTableCache::GetFileIdentity(const std::string& path)
{
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        throw std::filesystem::filesystem_error(
            "stat", path, std::error_code(errno, std::generic_category()));
    }
    return {st.st_dev, st.st_ino, std::filesystem::file_size(path),
            std::filesystem::last_write_time(path)};
}

std::shared_ptr<const ITable> TTableCache::Open(const std::string& path)
{
    const auto identity = GetFileIdentity(path);
    auto& entry = Tables_[path];
    if (!entry.Table || entry.Identity != identity) {
        // Keep the old entry if the open throws.
        entry = {std::make_shared<TParquetFile>(path), identity};
    }
    return entry.Table;
}

} // namespace NQdb
