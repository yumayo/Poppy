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
    std::cout << "Filename Unicode, separators, reserved names and limits passed\n";
}
