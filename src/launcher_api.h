#pragma once
#include <windows.h>

// Keep this C ABI stable. All application objects and allocations stay in the DLL.
using PoppyEntryV1 = int (WINAPI*)(HINSTANCE applicationModule, int show);
inline constexpr char poppyEntryName[] = "PoppyMainV1";
