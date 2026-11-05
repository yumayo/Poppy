#pragma once
#include "resource.h"

namespace poppy {
// Shell file lists carry UNC paths, without a Linux connection user. Import
// those paths using the destination's connection; private Poppy IDs stay intact.
ResourceId explorerClipboardPath(std::wstring_view path, const ResourceId& destination = {});
// POSIX backslashes cannot be represented by a Windows Shell file list.
std::optional<std::wstring> explorerShellPath(const ResourceId& path);
// Windows files can be imported into WSL; moves keep their connection.
bool canPasteExplorerResource(const ResourceId& source, const ResourceId& directory, bool cut = false);
}
