#include "limit_status.h"
#include "resource.h"
#include "wsl_protocol.h"
#include <charconv>
#include <istream>

namespace poppy {
WatchUsage readWatchBudget(std::istream& input) {
    WatchUsage usage{0, wsl::fallbackDirectoryWatches, 0, false, {}};
    std::string value, extra;
    if (!(input >> value) || (input >> extra)) return usage;
    uint64_t systemLimit = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), systemLimit);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return usage;
    usage.systemLimit = systemLimit; usage.limit = systemLimit / 2;
    return usage;
}
bool validWatchUsage(const WatchUsage& usage) {
    if (usage.count > usage.limit) return false;
    if (usage.systemLimit) return usage.limit == usage.systemLimit / 2;
    return usage.limit == 0 || usage.limit == wsl::fallbackDirectoryWatches;
}
std::wstring groupedCount(uint64_t value) {
    auto text = std::to_wstring(value);
    for (size_t offset = text.size(); offset > 3;) { offset -= 3; text.insert(offset, 1, L','); }
    return text;
}
namespace {
std::wstring ratio(uint64_t count, uint64_t limit) { return groupedCount(count) + L"/" + groupedCount(limit); }
std::wstring mib(size_t bytes) {
    const auto tenths = static_cast<uint64_t>(bytes) * 10 / (1024 * 1024);
    return groupedCount(tenths / 10) + L"." + std::to_wstring(tenths % 10);
}
}
std::wstring watchLimitMessage(const std::optional<WatchUsage>& usage) {
    std::wstring message;
    const auto reason = usage ? usage->reason : std::string{};
    if (reason == "poppyLimit") message = L"Poppy's directory watch limit was reached. Open a smaller folder or fewer projects, then restart Poppy.";
    else if (reason == "systemLimit") message = L"Linux could not allocate more watches. Close other applications using watches or increase fs.inotify.max_user_watches, then restart Poppy.";
    else if (reason == "drvfs") message = L"Windows-side changes under /mnt/ may not generate notifications. Use a folder in the WSL Linux filesystem for automatic watching.";
    else if (reason == "aliases") message = L"The limit for tracking opened symbolic-link targets was reached. Open fewer linked files, then restart Poppy.";
    else message = L"Some WSL directories could not be watched. Check the path and permissions in the Watch tooltip, then restart Poppy after resolving the error.";
    return message + L" F5 refreshes files manually; it does not restore automatic watching.";
}
LimitStatus watchLimitStatus(const std::optional<WatchUsage>& usage) {
    LimitStatus result;
    result.text = L"Watch " + (usage ? ratio(usage->count, usage->limit) : L"—/—");
    result.tooltip = L"WSL directory watches\n";
    if (!usage) { result.tooltip += L"Connect to WSL to see the current count and limit."; return result; }
    result.tooltip += ratio(usage->count, usage->limit) + L" directories registered.\nShared by all projects in this WSL distribution using the same connection user.";
    if (usage->systemLimit || !usage->limit) {
        result.tooltip += L"\nLinux user watch limit: " + groupedCount(usage->systemLimit) + L" (shared with other applications).";
        result.tooltip += L"\nPoppy's limit is half the Linux user watch limit read at helper startup. Restart Poppy after changing that setting.";
    } else result.tooltip += L"\nThe Linux user watch limit could not be read at helper startup. Using the fallback limit of " + groupedCount(wsl::fallbackDirectoryWatches) + L" directories.";
    result.warning = usage->limited || usage->count >= usage->limit;
    if (usage->limited) {
        result.text += L" !";
        result.tooltip += L"\n" + watchLimitMessage(usage);
        if (!usage->failedPath.empty()) result.tooltip += L"\nFailed path: " + resourceWide(usage->failedPath);
        if (!usage->error.empty()) result.tooltip += L"\nLinux error: " + resourceWide(usage->error);
    } else {
        if (usage->count >= usage->limit) result.tooltip += L"\nAt capacity. Additional directories cannot be watched.";
        result.tooltip += L"\nWatches wait for changes; this count does not cause periodic rescans.";
    }
    return result;
}
LimitStatus fileLimitStatus(const FileIndex* index, bool updating) {
    LimitStatus result;
    result.text = L"Files " + (index ? ratio(index->files.size(), maxIndexedFiles) : L"—/" + groupedCount(maxIndexedFiles)) + (updating ? L" …" : L"");
    result.tooltip = L"WSL file list for Find file (Ctrl+P)\n";
    if (index) {
        result.tooltip += ratio(index->files.size(), maxIndexedFiles) + L" searchable files.\nIndex memory budget: " + mib(index->storageBytes) + L"/" + groupedCount(maxIndexBytes / (1024 * 1024)) + L" MiB.";
        result.warning = !index->error.empty() || index->files.size() >= maxIndexedFiles || index->storageBytes >= maxIndexBytes;
        if (!index->error.empty()) result.tooltip += L"\n" + resourceWide(index->error);
    } else result.tooltip += L"The first file list has not completed.";
    result.tooltip += L"\nThe memory estimate covers retained names and allocation overhead, not total application memory or cumulative traffic.\nA scan visits up to " + groupedCount(maxIndexDirectories) + L" directories. Git internals and directory links are excluded; ignored files are included.";
    if (updating) result.tooltip += L"\nUpdating the list. Counts refer to the last completed scan.";
    if (result.warning) { result.text += L" !"; result.tooltip += L"\nA partial list remains searchable. Narrow the opened folder or press F5 to retry."; }
    return result;
}
LimitStatus treeLimitStatus(size_t count, size_t limit, size_t folderLimit, size_t partialFolders) {
    LimitStatus result{L"Tree " + ratio(count, limit), L"Explorer items retained for this project\n" + ratio(count, limit) + L" items, including cached children of collapsed folders.\nEach folder lists up to " + groupedCount(folderLimit) + L" entries.\nAt the limit, additional items are omitted from the Explorer.", count >= limit || partialFolders > 0};
    if (partialFolders) { result.text += L" !"; result.tooltip += L"\n" + groupedCount(partialFolders) + L" folders have incomplete listings. Press F5 to retry."; }
    return result;
}
}
