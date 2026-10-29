#pragma once
#include "file_search.h"
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>

namespace poppy {
struct WatchUsage {
    uint64_t count = 0, limit = 0, systemLimit = 0;
    bool limited = false;
    std::string reason;
    bool operator==(const WatchUsage& other) const {
        return count == other.count && limit == other.limit && systemLimit == other.systemLimit && limited == other.limited && reason == other.reason;
    }
};
struct LimitStatus {
    std::wstring text, tooltip;
    bool warning = false;
    bool operator==(const LimitStatus& other) const { return text == other.text && tooltip == other.tooltip && warning == other.warning; }
};
WatchUsage readWatchBudget(std::istream& systemLimit);
bool validWatchUsage(const WatchUsage& usage);
std::wstring groupedCount(uint64_t value);
LimitStatus watchLimitStatus(const std::optional<WatchUsage>& usage);
LimitStatus fileLimitStatus(const FileIndex* index, bool updating);
LimitStatus treeLimitStatus(size_t count, size_t limit, size_t folderLimit, size_t partialFolders = 0);
}
