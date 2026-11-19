#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include <sys/types.h>

#include <qdb/io/table.h>

namespace NQdb {

// Opened parquet tables by path, shared by planning and by the graphs a node
// materializes. An entry is reused while the file's identity is unchanged. Not
// thread-safe: the daemon's actors all run on its one event-loop thread.
class TTableCache {
public:
    // Throws on a file that cannot be opened.
    std::shared_ptr<const ITable> Open(const std::string& path);

private:
    // A replaced file can keep its mtime (cp -p, rsync -t), so the identity is wider.
    struct TFileIdentity {
        dev_t Device;
        ino_t Inode;
        std::uintmax_t Size;
        std::chrono::file_clock::time_point MTime;

        bool operator==(const TFileIdentity&) const = default;
    };

    struct TEntry {
        std::shared_ptr<const ITable> Table;
        TFileIdentity Identity;
    };

    static TFileIdentity GetFileIdentity(const std::string& path);

    std::unordered_map<std::string, TEntry> Tables_;
};

} // namespace NQdb
