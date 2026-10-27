#pragma once
#include <ctime>
#include <string>
#include <string_view>

namespace poppy {
constexpr size_t maxClipboardImageBytes = 64 * 1024 * 1024;
// Add the BMP file header missing from CF_DIB/CF_DIBV5, validating pixel bounds.
std::string clipboardDibBitmap(std::string_view dib);
std::wstring pastedImageName(const std::tm& localTime);
}

#ifdef _WIN32
#include <windows.h>
namespace poppy {
struct ClipboardImage { std::string bytes; bool dib = false; };
bool clipboardHasImage();
// Copy while the clipboard is open; conversion and file I/O happen on a worker.
ClipboardImage readClipboardImage(HWND owner);
std::string encodeClipboardPng(const ClipboardImage& image);
}
#endif
