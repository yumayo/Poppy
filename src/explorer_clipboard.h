#pragma once
#include "win.h"
#include "resource.h"

namespace poppy {
struct ExplorerClipboard {
    std::vector<ResourceId> paths;
    bool cut = false;
    DWORD sequence = 0;
};
bool clipboardHasFiles();
ExplorerClipboard readExplorerClipboard(HWND owner, const ResourceId& destination = {});
bool writeExplorerClipboard(HWND owner, const std::vector<ResourceId>& paths, bool cut);
}
