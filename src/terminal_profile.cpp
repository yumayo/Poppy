#include "terminal_profile.h"

namespace poppy {
const wchar_t* terminalShellName(TerminalShell shell) {
    return shell == TerminalShell::GitBash ? L"Git Bash" : L"PowerShell";
}
std::vector<std::wstring> terminalShellCommand(TerminalShell shell, const std::wstring& systemDirectory) {
    if (shell == TerminalShell::GitBash) return {L"C:\\Program Files\\Git\\bin\\bash.exe", L"--login", L"-i"};
    return {systemDirectory + L"\\WindowsPowerShell\\v1.0\\powershell.exe", L"-NoLogo", L"-NoProfile", L"-NoExit", L"-Command",
            L"[Console]::InputEncoding = [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()"};
}
}
