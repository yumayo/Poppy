#include "startup_options.h"
#include "resource.h"
#include <stdexcept>
#include <utility>

namespace poppy {
StartupOptions parseStartupOptions(const std::vector<std::wstring>& arguments) {
    StartupOptions result;
    for (size_t i = 0; i < arguments.size(); ++i) {
        const auto& argument = arguments[i];
        std::wstring project;
        if (argument == L"--project") {
            if (++i == arguments.size() || arguments[i].rfind(L"--", 0) == 0)
                throw std::invalid_argument("--project requires a directory.\n\nUsage: Poppy.exe --project \"C:\\work\\project\"");
            project = arguments[i];
        } else if (argument.rfind(L"--project=", 0) == 0) {
            project = argument.substr(10);
        } else if (arguments.size() == 1 && argument.rfind(L"-", 0) != 0) {
            // Keep the original Poppy.exe "directory" invocation working.
            project = argument;
        } else {
            throw std::invalid_argument("Unknown argument.\n\nUsage: Poppy.exe --project \"C:\\work\\project\"\nRepeat --project to add more directories.");
        }
        if (project.empty() || project.find(L'\0') != project.npos)
            throw std::invalid_argument("The project directory must not be empty or contain a null character.");
        result.projects.push_back(std::move(project));
    }
    return result;
}
std::wstring resolveStartupProjectPath(std::wstring_view project) {
    const auto text = resourceUtf8(project);
    // MinGW can treat a UNC root as drive-relative when making it absolute.
    // Detect WSL before that step, also preserving Linux symlink/.. semantics.
    if (wslUncLocation(text)) return std::wstring(project);
    const auto path = std::filesystem::absolute(std::filesystem::u8path(text)).lexically_normal().make_preferred();
    return resourceWide(path.u8string());
}
bool isAbsoluteStartupProjectPath(std::wstring_view project) {
    const auto text = resourceUtf8(project);
    return wslUncLocation(text).has_value() || std::filesystem::u8path(text).is_absolute();
}
}
