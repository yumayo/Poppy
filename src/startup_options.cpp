#include "startup_options.h"
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
}
