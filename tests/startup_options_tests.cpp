#include "startup_options.h"
#include "resource.h"
#include <iostream>
#include <stdexcept>

namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void options() {
    using poppy::parseStartupOptions;
    check(parseStartupOptions({}).projects.empty(), "No arguments should restore the workspace");
    const std::wstring path = L"C:\\work 日本語\\project=one\\";
    for (const auto& arguments : {std::vector<std::wstring>{L"--project", path},
            std::vector<std::wstring>{L"--project=" + path}, std::vector<std::wstring>{path}})
        check(parseStartupOptions(arguments).projects == std::vector<std::wstring>{path}, "A project path changed during parsing");
    check(parseStartupOptions({L"--project", L".\\relative", L"--project=\\\\server\\share\\project"}).projects ==
        std::vector<std::wstring>{L".\\relative", L"\\\\server\\share\\project"}, "Repeated projects lost their order");
    check(parseStartupOptions({L"--project", L"\\\\wsl.localhost\\Ubuntu\\home\\user\\project"}).projects.size() == 1,
        "A WSL UNC path was rejected");
    for (const auto& arguments : {std::vector<std::wstring>{L"--project"}, {L"--project="}, {L"--project", L""},
            {L"--unknown", L"C:\\work"}, {L"--project", L"--unknown"}, {L"--project", L"--project=C:\\work"},
            {L"C:\\first", L"C:\\second"}, {L"C:\\first", L"--project=C:\\second"}, {L""},
            {L"--project", std::wstring(L"C:\\bad\0path", 11)}}) {
        bool rejected = false;
        try { parseStartupOptions(arguments); } catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "Invalid arguments were accepted");
    }
}
void projectPaths() {
    using namespace poppy;
    for (const auto project : {
            L"\\\\wsl.localhost\\Ubuntu-22.04\\home\\user\\work\\sample-project",
            L"\\\\wsl$\\Ubuntu-22.04\\home\\user\\work\\sample-project",
            L"\\\\WSL.LOCALHOST\\Ubuntu\\home\\user\\日本語 project\\",
            L"//wsl.localhost/Ubuntu/home/user/project",
            L"\\\\wsl.localhost\\Ubuntu",
            L"\\\\wsl.localhost\\Ubuntu\\home\\user\\link\\..\\project"}) {
        const auto options = parseStartupOptions({L"--project", project});
        const auto resolved = resolveStartupProjectPath(options.projects.front());
        check(resolved == project, "Resolving a WSL startup path added a drive or changed its Linux path");
        check(isAbsoluteStartupProjectPath(resolved), "The IPC receiver rejected an absolute WSL path");
        const auto before = wslUncLocation(resourceUtf8(project)), after = wslUncLocation(resourceUtf8(resolved));
        check(before && after && before->distro == after->distro && before->root == after->root,
            "A resolved startup path no longer identifies the same WSL project");
    }
    check(!isAbsoluteStartupProjectPath(L"relative") && !isAbsoluteStartupProjectPath(L".\\relative"),
        "The IPC receiver accepted a caller-relative path");
    const auto relative = resolveStartupProjectPath(L"startup-relative/../日本語 project");
    const auto expected = (std::filesystem::current_path() / std::filesystem::u8path(u8"日本語 project")).make_preferred();
    check(relative == resourceWide(expected.u8string()) && isAbsoluteStartupProjectPath(relative),
        "A local relative path no longer resolves against the caller's directory");
#ifdef _WIN32
    check(resolveStartupProjectPath(L"C:\\work\\..\\日本語 project") == L"C:\\日本語 project" &&
        isAbsoluteStartupProjectPath(L"C:\\日本語 project"), "Resolving a Windows drive path changed");
#endif
}
}
int main() {
    try { options(); projectPaths(); std::cout << "Startup option and project path tests passed\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
