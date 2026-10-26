#include "wsl_dialog.h"
#include "render.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <algorithm>

namespace poppy {
namespace {
constexpr DWORD userGroup = 1, userEdit = 2;
// Keep the WSL picker's last folder separate from the Windows picker.
constexpr GUID wslFolderDialog = {0x5e44a301, 0xb07c, 0x4a68, {0x8a, 0xad, 0x7d, 0x02, 0xf3, 0xb8, 0x41, 0x39}};
using ShellText = std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)>;

void require(HRESULT result, const wchar_t* action) {
    if (FAILED(result)) throw std::runtime_error(utf8(std::wstring(action) + L"\n\n" + winError(static_cast<DWORD>(result))));
}

void setWslDefaultFolder(IFileOpenDialog* dialog, const std::vector<std::string>& distributions) {
    ComPtr<IShellItem> folder;
    HRESULT result = E_FAIL;
    // The server root lists distributions without starting every distribution
    // to populate custom shortcuts. Older Windows versions use the wsl$ alias.
    for (const auto* host : {L"\\\\wsl.localhost", L"\\\\wsl$"}) {
        result = SHCreateItemFromParsingName(host, nullptr, IID_PPV_ARGS(folder.put()));
        if (SUCCEEDED(result)) break;
    }
    if (FAILED(result) && !distributions.empty()) {
        for (const auto* host : {L"\\\\wsl.localhost\\", L"\\\\wsl$\\"}) {
            const auto path = std::wstring(host) + wide(distributions.front());
            result = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(folder.put()));
            if (SUCCEEDED(result)) break;
        }
    }
    require(result, L"Cannot browse WSL folders. Check that the distribution is available in File Explorer.");
    require(dialog->SetDefaultFolder(folder.get()), L"Cannot set the WSL browse location.");
}

ResourceId selectedFolder(IFileOpenDialog* dialog, IFileDialogCustomize* controls,
                          const std::vector<std::string>& distributions) {
    ComPtr<IShellItem> item;
    require(dialog->GetResult(item.put()), L"Cannot get the selected folder.");
    PWSTR value = nullptr;
    require(item->GetDisplayName(SIGDN_FILESYSPATH, &value), L"Cannot get the selected folder's path.");
    const ShellText path(value, CoTaskMemFree);
    const auto location = wslUncLocation(utf8(path.get()));
    if (!location) throw std::runtime_error("Choose a WSL folder under \\\\wsl.localhost or \\\\wsl$. Use Open Windows folder for a Windows folder.");
    const auto distro = std::find_if(distributions.begin(), distributions.end(), [&](const std::string& name) {
        return CompareStringOrdinal(wide(name).c_str(), -1, wide(location->distro).c_str(), -1, TRUE) == CSTR_EQUAL;
    });
    if (distro == distributions.end()) throw std::runtime_error("The WSL distribution is not registered: " + location->distro);
    value = nullptr;
    require(controls->GetEditBoxText(userEdit, &value), L"Cannot read the WSL user name.");
    const ShellText user(value, CoTaskMemFree);
    return ResourceId::wsl(*distro, location->root, user ? utf8(user.get()) : std::string{});
}
}

std::optional<ResourceId> chooseWslFolder(HWND owner, const std::vector<std::string>& distributions) {
    try {
        ComPtr<IFileOpenDialog> dialog;
        require(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())), L"Cannot create the folder picker.");
        DWORD options = 0;
        require(dialog->GetOptions(&options), L"Cannot read the folder picker options.");
        require(dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR), L"Cannot configure the folder picker.");
        require(dialog->SetTitle(L"Open WSL folder"), L"Cannot set the folder picker title.");
        require(dialog->SetOkButtonLabel(L"Open"), L"Cannot set the folder picker button.");
        require(dialog->SetClientGuid(wslFolderDialog), L"Cannot set the folder picker history.");
        setWslDefaultFolder(dialog.get(), distributions);

        ComPtr<IFileDialogCustomize> controls;
        require(dialog->QueryInterface(IID_PPV_ARGS(controls.put())), L"Cannot configure the WSL user field.");
        require(controls->StartVisualGroup(userGroup, L"WSL user (optional)"), L"Cannot add the WSL user field.");
        require(controls->AddEditBox(userEdit, L""), L"Cannot add the WSL user field.");
        require(controls->EndVisualGroup(), L"Cannot add the WSL user field.");

        for (;;) {
            const auto result = dialog->Show(owner);
            if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
            require(result, L"Cannot show the folder picker.");
            try { return selectedFolder(dialog.get(), controls.get(), distributions); }
            catch (const std::exception& error) { MessageBoxW(owner, wide(error.what()).c_str(), L"Open WSL folder", MB_OK | MB_ICONWARNING); }
        }
    } catch (const std::exception& error) {
        MessageBoxW(owner, wide(error.what()).c_str(), L"Open WSL folder", MB_OK | MB_ICONWARNING);
        return {};
    }
}
}
