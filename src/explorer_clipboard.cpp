#include "explorer_clipboard.h"
#include "explorer_clipboard_paths.h"
#include <shellapi.h>
#include <shlobj.h>
#include <cstring>
#include <stdexcept>

namespace poppy {
namespace {
UINT resourceFormat() { return RegisterClipboardFormatW(L"Poppy.Explorer.Resources.v1"); }
UINT effectFormat() { return RegisterClipboardFormatW(L"Preferred DropEffect"); }
bool put(UINT format, const void* bytes, size_t size) {
    const auto memory = GlobalAlloc(GMEM_MOVEABLE, size); if (!memory) return false;
    const auto data = GlobalLock(memory); if (!data) { GlobalFree(memory); return false; }
    std::memcpy(data, bytes, size); GlobalUnlock(memory);
    if (SetClipboardData(format, memory)) return true;
    GlobalFree(memory); return false;
}
}
bool clipboardHasFiles() { return IsClipboardFormatAvailable(resourceFormat()) || IsClipboardFormatAvailable(CF_HDROP); }
ExplorerClipboard readExplorerClipboard(HWND owner, const ResourceId& destination) {
    ExplorerClipboard result;
    if (!OpenClipboard(owner)) return result;
    struct Close { ~Close() { CloseClipboard(); } } close;
    result.sequence = GetClipboardSequenceNumber();
    const auto memory = GetClipboardData(resourceFormat());
    if (memory) {
        const auto size = GlobalSize(memory);
        if (!size || size > 4 * 1024 * 1024) return result;
        const auto data = static_cast<const char*>(GlobalLock(memory)); if (!data) return result;
        try {
            size_t offset = 0;
            while (offset < size && data[offset]) {
                const auto end = static_cast<const char*>(std::memchr(data + offset, 0, size - offset));
                if (!end || result.paths.size() >= 50000) throw std::runtime_error("Invalid file clipboard");
                auto path = ResourceId::parse(std::string_view(data + offset, end - data - offset));
                if (!path.is_absolute()) throw std::runtime_error("Invalid file clipboard path");
                result.paths.push_back(std::move(path)); offset = end - data + 1;
            }
        } catch (const std::exception&) { result.paths.clear(); }
        GlobalUnlock(memory);
    } else if (const auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP))) {
        const auto count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
        if (count > 50000) return result;
        for (UINT i = 0; i < count; ++i) {
            const auto length = DragQueryFileW(drop, i, nullptr, 0);
            if (!length || length > 32767) { result.paths.clear(); return result; }
            std::wstring path(length + 1, 0); DragQueryFileW(drop, i, path.data(), length + 1); path.resize(length);
            try {
                auto resource = explorerClipboardPath(path, destination);
                if (!resource.is_absolute()) { result.paths.clear(); return result; }
                result.paths.push_back(std::move(resource));
            } catch (const std::exception&) { result.paths.clear(); return result; }
        }
    }
    const auto effect = GetClipboardData(effectFormat());
    if (effect && GlobalSize(effect) >= sizeof(DWORD)) {
        const auto data = static_cast<const DWORD*>(GlobalLock(effect));
        if (data) { result.cut = *data == DROPEFFECT_MOVE; GlobalUnlock(effect); }
    }
    return result;
}
bool writeExplorerClipboard(HWND owner, const std::vector<ResourceId>& paths, bool cut) {
    std::string resources;
    std::wstring native;
    bool shellPaths = true;
    for (const auto& path : paths) {
        resources += path.serialized(); resources.push_back(0);
        const auto name = explorerShellPath(path);
        if (!name) shellPaths = false;
        else { native += *name; native.push_back(0); }
    }
    resources.push_back(0); native.push_back(0);
    if (resources.size() > 4 * 1024 * 1024 || !OpenClipboard(owner)) return false;
    struct Close { ~Close() { CloseClipboard(); } } close;
    if (!EmptyClipboard()) return false;
    if (paths.empty()) return true;
    if (!put(resourceFormat(), resources.data(), resources.size())) return false;
    const DWORD effect = cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
    if (!put(effectFormat(), &effect, sizeof(effect))) return false;
    if (shellPaths) {
        std::vector<char> bytes(sizeof(DROPFILES) + native.size() * sizeof(wchar_t));
        DROPFILES header{}; header.pFiles = sizeof(DROPFILES); header.fWide = TRUE;
        std::memcpy(bytes.data(), &header, sizeof(header));
        std::memcpy(bytes.data() + sizeof(header), native.data(), native.size() * sizeof(wchar_t));
        if (!put(CF_HDROP, bytes.data(), bytes.size())) return false;
    }
    return true;
}
}
