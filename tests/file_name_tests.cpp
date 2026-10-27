#include "file_name.h"
#include <cstdlib>
#include <iostream>
using namespace poppy;
int main() {
    std::wstring error;
    for (const auto name : {L"file.cpp", L"日本語 ファイル.txt", L".gitignore", L"COM10.txt", L"README", L"a.b.c"}) {
        if (!validFileName(name, error)) { std::cerr << "Valid filename rejected\n"; return 1; }
    }
    for (const auto name : {L"", L".", L"..", L"a/b", L"a\\b", L"C:other", L"*.txt", L"file.", L"file ", L"CON", L"nul.txt", L"LPT9.log", L"com¹.txt", L"CON .txt", L"CONIN$", L"bad\nname"}) {
        if (validFileName(name, error) || error.empty()) { std::cerr << "Invalid filename accepted\n"; return 1; }
    }
    if (validFileName(std::wstring(256, L'x'), error)) return 1;
    for (const bool posix : {false, true}) {
        const auto file = parseCreationPath(L"path/to/日本語 file.md", false, posix, error);
        const auto folder = parseCreationPath(L"path/to/folder/", false, posix, error);
        const auto explicitFolder = parseCreationPath(L"path/to/folder", true, posix, error);
        if (!file || file->directory || file->components != std::vector<std::wstring>{L"path", L"to", L"日本語 file.md"} ||
            !folder || !folder->directory || folder->components.back() != L"folder" ||
            !explicitFolder || !explicitFolder->directory) return 1;
        for (const auto name : {L"", L"/", L"/absolute/file", L"../escape.txt", L"a/../file", L"a/./file", L"a//file", L"a//", L"a/.."}) {
            if (parseCreationPath(name, false, posix, error) || error.empty()) { std::cerr << "Invalid creation path accepted\n"; return 1; }
        }
        if (parseCreationPath(L"a/" + std::wstring(256, L'x'), false, posix, error) ||
            parseCreationPath(std::wstring(maxCreationPathLength + 1, L'x'), false, posix, error)) return 1;
        // A complete path may exceed the old single-name input limit.
        const auto longPath = std::wstring(200, L'a') + L"/" + std::wstring(200, L'b') + L"/file.txt";
        if (!parseCreationPath(longPath, false, posix, error)) return 1;
        if (parseCreationPath(std::wstring_view(L"a/\0/file", 8), false, posix, error)) return 1;
    }
    for (const auto name : {L"a/CON/file", L"C:/absolute", L"a\\b/file", L"a/file. ", L"a/*.txt"}) {
        if (parseCreationPath(name, false, false, error)) return 1;
    }
    if (!parseCreationPath(L"CON/back\\slash:*.txt", false, true, error) ||
        parseCreationPath(L"folder/" + std::wstring(86, L'あ'), false, true, error)) return 1;
    std::cout << "Filename and creation path validation passed\n";
}
