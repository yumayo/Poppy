#pragma once
#include <cstdint>
#include <string>
#include <utility>

namespace poppy {
// A load still running in one editor pane. Restarting it would discard its
// result and queue the replacement behind newer work, so a steady stream of
// file changes could keep the pane loading. Background reloads requested in
// the meantime collapse into one follow-up after the running load lands.
class PendingLoad {
public:
    void start(uint64_t serial, std::wstring key) { serial_ = serial; key_ = std::move(key); active_ = true; reload_ = false; }
    // Defers a background reload while the same load is still current.
    bool defer(uint64_t serial, const std::wstring& key) {
        if (!active_ || serial != serial_ || key != key_) return false;
        reload_ = true; return true;
    }
    // Ends the load; true when a deferred reload is due.
    bool finish() { const bool reload = active_ && reload_; *this = {}; return reload; }
    // Explicit refreshes restart the load instead of waiting for it.
    void cancel() { *this = {}; }
private:
    uint64_t serial_ = 0;
    std::wstring key_;
    bool active_ = false, reload_ = false;
};
}
