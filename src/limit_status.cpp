#include "limit_status.h"
#include "resource.h"
#include "wsl_protocol.h"

namespace poppy {
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
LimitStatus watchLimitStatus(const std::optional<WatchUsage>& usage) {
    LimitStatus result;
    result.text = L"Watch " + (usage ? ratio(usage->count, usage->limit) : L"—/" + groupedCount(wsl::maxDirectoryWatches));
    result.tooltip = L"WSL directory watches\n";
    if (!usage) { result.tooltip += L"Connect to WSL to see the current count."; return result; }
    result.tooltip += ratio(usage->count, usage->limit) + L" directories registered.\nShared by all projects in this WSL distribution using the same connection user.";
    if (usage->systemLimit) result.tooltip += L"\nLinux user watch limit: " + groupedCount(usage->systemLimit) + L" (shared with other applications).";
    result.warning = usage->limited || usage->count >= usage->limit;
    if (usage->limited) {
        result.text += L" !";
        if (usage->reason == "poppyLimit") result.tooltip += L"\nPoppy's directory watch limit was reached.";
        else if (usage->reason == "systemLimit") result.tooltip += L"\nLinux could not allocate more watches. The user quota or kernel resources may be exhausted.";
        else if (usage->reason == "drvfs") result.tooltip += L"\nWindows-side changes under /mnt/ may not generate notifications.";
        else if (usage->reason == "aliases") result.tooltip += L"\nThe limit for tracking opened symbolic-link targets was reached.";
        else result.tooltip += L"\nSome directories could not be watched.";
        result.tooltip += L"\nChanges without notifications need F5 to refresh.";
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
