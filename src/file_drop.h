#pragma once
#include "win.h"
#include <oleidl.h>

namespace poppy {
// Both CF_HDROP clipboard data and OLE drops use this Shell file list.
std::vector<std::wstring> shellFilePaths(HGLOBAL memory);

class FileDropTarget final : public IDropTarget {
public:
    using Hover = std::function<bool(POINT, const std::vector<std::wstring>&)>;
    using Leave = std::function<void()>;
    using DropFiles = std::function<void(const std::vector<std::wstring>&)>;
    FileDropTarget(Hover hover, Leave leave, DropFiles drop)
        : hover_(std::move(hover)), leave_(std::move(leave)), drop_(std::move(drop)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keys, POINTL point, DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL point, DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragLeave() override;
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keys, POINTL point, DWORD* effect) override;
private:
    ~FileDropTarget() = default;
    void read(IDataObject* data);
    ULONG references_ = 1;
    Hover hover_;
    Leave leave_;
    DropFiles drop_;
    std::vector<std::wstring> files_;
};
}
