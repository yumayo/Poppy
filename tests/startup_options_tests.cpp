#include "startup_options.h"
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
}
int main() {
    try { options(); std::cout << "Startup option tests passed\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
