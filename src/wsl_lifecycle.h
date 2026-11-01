#pragma once
#include <string>

namespace poppy::wsl {
// Called by the relay in the WSL host, where Docker and the Linux processes
// were launched. The dedicated socket is the ownership key for both.
void cleanupTerminalSocket(const std::string& socket);
}
