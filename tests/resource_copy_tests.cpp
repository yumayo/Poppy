#include "resource_copy.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;
using namespace poppy;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Fixture {
    fs::path root = fs::temp_directory_path() / ("poppy-copy-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { fs::create_directory(root); }
    ~Fixture() { std::error_code error; fs::remove_all(root, error); }
};
void write(const fs::path& path, const std::string& bytes) { std::ofstream file(path, std::ios::binary); file.write(bytes.data(), bytes.size()); check(bool(file), "Cannot write fixture"); }
std::string read(const fs::path& path) { std::ifstream file(path, std::ios::binary); return {std::istreambuf_iterator<char>(file), {}}; }
void fails(const fs::path& source, const fs::path& destination) {
    bool failed = false;
    try { copyResourcePath(source, destination); } catch (const std::exception&) { failed = true; }
    check(failed, "An invalid copy succeeded");
}
void copying() {
    Fixture fixture;
    const auto source = fixture.root / fs::u8path("source 日本語"), target = fixture.root / "target";
    fs::create_directories(source / "nested" / "empty"); fs::create_directory(target);
    const std::string bytes("binary\0data\r\n", 13);
    write(source / "nested" / "data.bin", bytes);
    copyResourcePath(source, target / source.filename());
    check(read(target / source.filename() / "nested" / "data.bin") == bytes &&
        fs::is_empty(target / source.filename() / "nested" / "empty") && read(source / "nested" / "data.bin") == bytes,
        "Folder copy lost binary contents, empty folders or its source");
    write(target / "collision", "keep"); fails(source / "nested" / "data.bin", target / "collision");
    check(read(target / "collision") == "keep", "Copy overwrote an existing file");
    fails(source, target / source.filename());
    fails(source, source / "nested" / "recursive");
    fails(source, source); fails(fixture.root / "missing", target / "missing");
    check(!fs::exists(source / "nested" / "recursive") && !fs::exists(target / "missing"), "A rejected copy left a partial destination");
    const auto large = source / "large.bin"; write(large, "large"); fs::resize_file(large, 20 * 1024 * 1024);
    copyResourcePath(large, target / "large.bin");
    check(fs::file_size(target / "large.bin") == 20 * 1024 * 1024, "Copy incorrectly used the editor preview size limit");
#ifndef _WIN32
    fs::permissions(source / "nested" / "data.bin", fs::perms::owner_all);
    copyResourcePath(source / "nested" / "data.bin", target / "executable");
    check((fs::status(target / "executable").permissions() & fs::perms::owner_exec) != fs::perms::none, "Copy lost executable permission");
    fs::create_directory_symlink(source, fixture.root / "alias");
    fails(source, fixture.root / "alias" / "nested" / "recursive");
    fails(source / "nested" / "data.bin", fixture.root / "alias" / "file");
    fs::create_symlink("../nested", source / "nested" / "loop");
    fs::create_symlink("missing", source / "broken");
    copyResourcePath(source, target / "links");
    check(fs::is_symlink(target / "links" / "nested" / "loop") &&
        fs::read_symlink(target / "links" / "nested" / "loop") == "../nested" && fs::is_symlink(target / "links" / "broken"),
        "Copy followed a cyclic or dangling symlink");
    fs::permissions(source / "nested", fs::perms::owner_read | fs::perms::owner_exec);
    fails(source, target / source.filename());
    fs::permissions(source / "nested", fs::perms::owner_all);
    check(mkfifo((source / "nested" / "pipe").c_str(), 0600) == 0, "Cannot create an unsupported copy fixture");
    fails(source, target / "partial");
    check(!fs::exists(target / "partial"), "A failed copy published partial contents");
    fs::remove(source / "nested" / "pipe");
#endif
    for (const auto& directory : {fixture.root, source, source / "nested", target})
        for (const auto& entry : fs::directory_iterator(directory)) check(entry.path().filename().string().rfind(".poppy-copy-", 0) != 0, "Copy leaked staging data");
}
#ifdef _WIN32
void windowsFolderTimestamps() {
    Fixture fixture;
    const auto source = fixture.root / fs::u8path("project A 日本語") / "folder", target = fixture.root / "project B";
    fs::create_directories(source / "nested" / "empty"); fs::create_directory(target);
    write(source / "nested" / "data.bin", "folder contents");
    // Use native timestamps with subsecond precision. Setting a directory's
    // timestamp through MinGW's filesystem API would fail in the fixture too.
    for (const auto& path : {source / "nested" / "data.bin", source / "nested" / "empty", source / "nested", source}) {
        const auto handle = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        check(handle != INVALID_HANDLE_VALUE, "Cannot open timestamp fixture");
        const FILETIME timestamp{123456789, 31000000};
        const auto success = SetFileTime(handle, nullptr, nullptr, &timestamp);
        CloseHandle(handle);
        check(success != FALSE, "Cannot set timestamp fixture");
    }
    const auto destination = target / source.filename();
    copyResourcePath(source, destination);
    for (const auto& relative : {fs::path{}, fs::path("nested"), fs::path("nested/empty"), fs::path("nested/data.bin")}) {
        const auto from = relative.empty() ? source : source / relative;
        const auto to = relative.empty() ? destination : destination / relative;
        WIN32_FILE_ATTRIBUTE_DATA before{}, after{};
        check(GetFileAttributesExW(from.c_str(), GetFileExInfoStandard, &before) &&
            GetFileAttributesExW(to.c_str(), GetFileExInfoStandard, &after), "Cannot read copied timestamps");
        check(CompareFileTime(&before.ftLastWriteTime, &after.ftLastWriteTime) == 0,
            "Cross-project folder copy lost a file or directory timestamp");
    }
    check(read(destination / "nested" / "data.bin") == "folder contents" && fs::is_empty(destination / "nested" / "empty") &&
        read(source / "nested" / "data.bin") == "folder contents", "Cross-project folder copy changed contents or its source");
    for (const auto& entry : fs::directory_iterator(target))
        check(entry.path() == destination, "Cross-project folder copy leaked staging data");
}
#endif
}
int main() {
    try {
        copying();
#ifdef _WIN32
        windowsFolderTimestamps();
#endif
        std::cout << "Resource copy tests passed\n"; return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
