#pragma once

// Separate from the interactive terminal and filesystem connection. EOF on the
// Windows-owned input pipe closes the socket, including after an abrupt exit.
namespace poppy::wsl { int serveTerminalEvents(); }
