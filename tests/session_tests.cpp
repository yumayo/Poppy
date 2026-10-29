#include "session.h"
#include "command_line.h"
#include <iostream>
#include <stdexcept>

namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
}
int main() {
    try {
        using poppy::projectDirectoryName;
        check(projectDirectoryName("D:\\repository\\yumayo\\PoppyWorktrees\\PoppyDirect2D") ==
              "-d-repository-yumayo-poppyworktrees-poppydirect2d", "Windows project directory format changed");
        check(projectDirectoryName("d:/repository/yumayo/PoppyWorktrees/PoppyDirect2D/") ==
              "-d-repository-yumayo-poppyworktrees-poppydirect2d", "Alternate separators changed the directory format");
        check(projectDirectoryName("\\\\server\\share\\project") == "-server-share-project", "UNC project format is incorrect");
        check(projectDirectoryName("C:\\") == "-c", "Drive-root project format is incorrect");
        const auto longName = projectDirectoryName("d:\\" + std::string(1000, 'a'));
        check(longName.size() <= 137, "Long paths exceed the directory component limit");
        check(longName != projectDirectoryName("d:\\" + std::string(999, 'a') + 'b'), "Truncating long paths lost their identity");
        check(projectDirectoryName("d:/a.b") == projectDirectoryName("d:/a-b"), "The collision fixture no longer collides");
        check(poppy::projectPathHash("d:/a.b") != poppy::projectPathHash("d:/a-b"), "Colliding readable names need different suffixes");
        const auto bash = poppy::terminalShellCommand(poppy::TerminalShell::GitBash, L"C:\\Windows\\System32");
        check(poppy::processCommandLine(bash) == L"\"C:\\Program Files\\Git\\bin\\bash.exe\" --login -i",
              "Git Bash needs a quoted executable path and an interactive login shell");
        const auto powershell = poppy::terminalShellCommand(poppy::TerminalShell::PowerShell, L"D:\\Windows\\System32");
        check(powershell.front() == L"D:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe" &&
              powershell.back().find(L"UTF8Encoding") != std::wstring::npos,
              "PowerShell lost the system directory or UTF-8 initialization");
        std::cout << "Session path tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
