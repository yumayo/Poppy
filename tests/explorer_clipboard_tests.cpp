#include "explorer_clipboard_paths.h"
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#include "explorer_clipboard.h"
#include <shellapi.h>
#include <shlobj.h>
#include <cstring>
#endif

using namespace poppy;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void paths() {
    const auto target = ResourceId::wsl("Ubuntu", "/home/alice/project", "alice");
    for (const auto* host : {L"wsl.localhost", L"wsl$", L"WSL.LOCALHOST"}) {
        for (const auto* name : {L"file 日本語.txt", L"folder 日本語", L"A.txt", L"a.txt"}) {
            const auto native = L"\\\\" + std::wstring(host) + L"\\ubuntu\\home\\alice\\" + name;
            const auto path = explorerClipboardPath(native, target);
            check(path.sameAuthority(target) && path.u8string() == "/home/alice/" + resourceUtf8(name),
                "A Shell WSL path lost its destination user, canonical distro or Linux filename");
            const auto shell = explorerShellPath(path);
            check(shell && explorerClipboardPath(*shell, target) == path, "A WSL file or folder failed the Shell round trip");
        }
    }
    const auto other = explorerClipboardPath(L"\\\\wsl$\\Debian\\home\\alice\\file.txt", target);
    check(other.wsl() && other.location().distro == "Debian" && other.location().user.empty() && !other.sameAuthority(target),
        "Import silently changed a different WSL distribution");
    const auto defaultUser = explorerClipboardPath(L"\\\\wsl.localhost\\Ubuntu\\home\\alice\\file.txt");
    check(defaultUser.wsl() && defaultUser.location().user.empty(), "A destination-free import invented a WSL user");
    const ResourceId share = std::filesystem::path(L"\\\\server\\share\\file.txt");
    check(!explorerClipboardPath(share.native(), target).wsl() && explorerShellPath(share) == share.native(),
        "An ordinary Windows share was converted to a WSL resource");
    const auto backslash = target / std::string_view("back\\slash.txt");
    check(!explorerShellPath(backslash) && ResourceId::parse(backslash.serialized()) == backslash,
        "A POSIX backslash was exposed as a Shell path or lost in a private resource ID");
    check(explorerShellPath(target) == L"\\\\wsl.localhost\\Ubuntu\\home\\alice\\project",
        "A WSL folder was not published as a Windows UNC path");
#ifdef _WIN32
    const ResourceId windows = std::filesystem::path(L"C:\\work 日本語\\file.txt");
    check(explorerShellPath(windows) == windows.native() && explorerClipboardPath(windows.native(), target) == windows,
        "A Windows file was changed by the file clipboard conversion");
#endif
}
#ifdef _WIN32
void putShellFiles(HWND owner, const std::vector<std::wstring>& paths) {
    std::wstring names;
    for (const auto& path : paths) { names += path; names.push_back(0); }
    names.push_back(0);
    DROPFILES header{}; header.pFiles = sizeof(header); header.fWide = TRUE;
    const auto memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(header) + names.size() * sizeof(wchar_t));
    check(memory != nullptr, "Cannot allocate Shell clipboard data");
    const auto bytes = static_cast<char*>(GlobalLock(memory));
    if (!bytes) { GlobalFree(memory); throw std::runtime_error("Cannot lock Shell clipboard data"); }
    std::memcpy(bytes, &header, sizeof(header));
    std::memcpy(bytes + sizeof(header), names.data(), names.size() * sizeof(wchar_t));
    GlobalUnlock(memory);
    if (!OpenClipboard(owner)) { GlobalFree(memory); throw std::runtime_error("Cannot open clipboard"); }
    const bool success = EmptyClipboard() && SetClipboardData(CF_HDROP, memory);
    CloseClipboard();
    if (!success) GlobalFree(memory);
    check(success, "Cannot publish Shell clipboard data");
}
void clipboard() {
    const auto window = CreateWindowExW(0, L"STATIC", L"Poppy clipboard test", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    check(window != nullptr, "Cannot create clipboard owner");
    struct Cleanup { HWND window; ~Cleanup() { writeExplorerClipboard(window, {}, false); DestroyWindow(window); } } cleanup{window};
    const auto target = ResourceId::wsl("Ubuntu", "/home/alice/project", "alice");
    const std::vector<ResourceId> windows{std::filesystem::path(L"C:\\work 日本語\\file.txt"), std::filesystem::path(L"C:\\work 日本語\\folder")};
    const std::vector<ResourceId> wsl{target / std::string_view("file 日本語.txt"), target / std::string_view("folder 日本語")};
    for (const auto& paths : {windows, wsl}) {
        for (const bool cut : {false, true}) {
            check(writeExplorerClipboard(window, paths, cut) && clipboardHasFiles() && IsClipboardFormatAvailable(CF_HDROP),
                "Copy did not publish files for Poppy and File Explorer");
            const auto result = readExplorerClipboard(window, target);
            check(result.paths == paths && result.cut == cut && result.sequence == GetClipboardSequenceNumber(),
                "Private clipboard data lost files, folders, connection authority or copy/cut state");
            check(OpenClipboard(window) != FALSE, "Cannot inspect Shell file data");
            const auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
            bool same = drop && DragQueryFileW(drop, 0xffffffff, nullptr, 0) == paths.size();
            for (UINT i = 0; same && i < paths.size(); ++i) {
                const auto length = DragQueryFileW(drop, i, nullptr, 0);
                std::wstring name(length + 1, 0); DragQueryFileW(drop, i, name.data(), length + 1); name.resize(length);
                same = name == explorerShellPath(paths[i]);
            }
            CloseClipboard(); check(same, "Shell file clipboard data omitted or changed a file/folder path");
        }
    }
    putShellFiles(window, {L"\\\\wsl.localhost\\ubuntu\\home\\alice\\file 日本語.txt", L"\\\\wsl$\\Ubuntu\\home\\alice\\folder 日本語"});
    const auto imported = readExplorerClipboard(window, target);
    check(imported.paths == std::vector<ResourceId>{target.at("/home/alice/file 日本語.txt"), target.at("/home/alice/folder 日本語")} && !imported.cut,
        "File Explorer WSL files/folders did not use the selected connection user");
    putShellFiles(window, {windows[0].native(), windows[1].native()});
    check(readExplorerClipboard(window, target).paths == windows, "File Explorer Windows files/folders changed during import");
    const auto otherUser = ResourceId::wsl("Ubuntu", "/home/bob/file.txt", "bob");
    check(writeExplorerClipboard(window, {otherUser}, false) && readExplorerClipboard(window, target).paths == std::vector<ResourceId>{otherUser},
        "Poppy private clipboard data silently changed an explicit source user");
    const auto backslash = target / std::string_view("back\\slash.txt");
    check(writeExplorerClipboard(window, {backslash}, false) && !IsClipboardFormatAvailable(CF_HDROP) &&
        readExplorerClipboard(window, target).paths == std::vector<ResourceId>{backslash}, "A Linux backslash was not kept in the Poppy-only clipboard");
    putShellFiles(window, {L"C:\\work\\file.txt", L"relative.txt"});
    check(readExplorerClipboard(window, target).paths.empty(), "An invalid Shell file list was partially accepted");
    check(writeExplorerClipboard(window, {}, false) && !clipboardHasFiles(), "Clearing the file clipboard kept stale paths");
}
#endif
}
int main() {
    try {
        paths();
#ifdef _WIN32
        clipboard();
#endif
        std::cout << "Explorer clipboard tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
