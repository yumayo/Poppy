#pragma once
#include <string>
#include <vector>

namespace poppy {
enum class TerminalShell { PowerShell, GitBash };
const wchar_t* terminalShellName(TerminalShell shell);
// systemDirectory is supplied by GetSystemDirectoryW on Windows.
std::vector<std::wstring> terminalShellCommand(TerminalShell shell, const std::wstring& systemDirectory);
}
