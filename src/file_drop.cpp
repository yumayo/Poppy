#include "file_drop.h"
#include <shellapi.h>
#include <shlobj.h>
#include <cstring>

namespace poppy {
std::vector<std::wstring> shellFilePaths(HGLOBAL memory) {
    std::vector<std::wstring> paths;
    const auto size = GlobalSize(memory);
    if (size < sizeof(DROPFILES) || size > 4 * 1024 * 1024) return paths;
    const auto bytes = static_cast<const char*>(GlobalLock(memory));
    if (!bytes) return paths;
    DROPFILES header{}; std::memcpy(&header, bytes, sizeof(header));
    const size_t unit = header.fWide ? sizeof(wchar_t) : 1;
    bool terminated = false;
    if (header.pFiles >= sizeof(header) && header.pFiles <= size && header.pFiles % unit == 0 && (size - header.pFiles) / unit >= 2) {
        // Do not pass a truncated file list to the Shell parser.
        for (size_t i = header.pFiles; i + 2 * unit <= size; i += unit) {
            bool zero = true;
            for (size_t j = 0; j < 2 * unit; ++j) if (bytes[i + j]) { zero = false; break; }
            if (zero) { terminated = true; break; }
        }
    }
    GlobalUnlock(memory);
    if (!terminated) return paths;
    const auto drop = static_cast<HDROP>(memory);
    const auto count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
    if (count > 50000) return {};
    for (UINT i = 0; i < count; ++i) {
        const auto length = DragQueryFileW(drop, i, nullptr, 0);
        if (!length || length > 32767) return {};
        std::wstring path(length + 1, 0);
        if (DragQueryFileW(drop, i, path.data(), length + 1) != length) return {};
        path.resize(length);
        if (!std::filesystem::path(path).is_absolute()) return {};
        paths.push_back(std::move(path));
    }
    return paths;
}
HRESULT FileDropTarget::QueryInterface(REFIID iid, void** object) {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (iid != IID_IUnknown && iid != IID_IDropTarget) return E_NOINTERFACE;
    *object = static_cast<IDropTarget*>(this); AddRef(); return S_OK;
}
ULONG FileDropTarget::Release() { const auto count = --references_; if (!count) delete this; return count; }
void FileDropTarget::read(IDataObject* data) {
    files_.clear();
    FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (!data || FAILED(data->GetData(&format, &medium))) return;
    struct Release { STGMEDIUM& medium; ~Release() { ReleaseStgMedium(&medium); } } release{medium};
    if (medium.tymed == TYMED_HGLOBAL) files_ = shellFilePaths(medium.hGlobal);
}
HRESULT FileDropTarget::DragEnter(IDataObject* data, DWORD keys, POINTL point, DWORD* effect) {
    try { read(data); }
    catch (...) { if (effect) *effect = DROPEFFECT_NONE; DragLeave(); return E_UNEXPECTED; }
    return DragOver(keys, point, effect);
}
HRESULT FileDropTarget::DragOver(DWORD, POINTL point, DWORD* effect) {
    if (!effect) return E_POINTER;
    try {
        const bool accepted = (*effect & DROPEFFECT_COPY) && !files_.empty() && hover_ && hover_({point.x, point.y}, files_);
        *effect = accepted ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        if (!accepted && leave_) leave_();
        return S_OK;
    } catch (...) { *effect = DROPEFFECT_NONE; DragLeave(); return E_UNEXPECTED; }
}
HRESULT FileDropTarget::DragLeave() { files_.clear(); if (leave_) leave_(); return S_OK; }
HRESULT FileDropTarget::Drop(IDataObject* data, DWORD keys, POINTL point, DWORD* effect) {
    if (!effect) return E_POINTER;
    try {
        // Read again: a source can render its file list only when dropped.
        read(data);
        const auto result = DragOver(keys, point, effect);
        if (SUCCEEDED(result) && *effect == DROPEFFECT_COPY && drop_) drop_(files_);
        DragLeave(); return result;
    } catch (...) { *effect = DROPEFFECT_NONE; DragLeave(); return E_UNEXPECTED; }
}
}
