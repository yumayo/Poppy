#pragma once
#include <string_view>

namespace poppy {
struct FileChange {
    bool relevant = false, files = false, content = false;
};
// Paths use '/' separators. POSIX backslashes remain part of a filename.
// Git locks, objects and reflogs must not trigger another Git status scan.
FileChange classifyFileChange(std::string_view path, bool topology, bool windows = false);
}
