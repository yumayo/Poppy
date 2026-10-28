#pragma once
#include "win.h"
#include "startup_options.h"

namespace poppy {
constexpr ULONG_PTR startupCopyData = 0x50505902;
StartupOptions parseStartupCommandLine(const wchar_t* commandLine);

// One workspace per Windows session and storage profile (including test APPDATA).
class SingleInstance {
public:
    explicit SingleInstance(const fs::path& stateDirectory);
    ~SingleInstance();
    // Returns true for the owner, false after the running owner accepts a request.
    bool acquireOrForward(const StartupOptions& options);
    void publish(HWND window);
    void unpublish(HWND window);
private:
    Handle mutex_;
    std::wstring name_;
    bool owned_ = false;
};
}
