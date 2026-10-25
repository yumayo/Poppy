#pragma once
#include "text.h"
#include <deque>

namespace poppy {
// Byte offsets always point to UTF-8 boundaries; CRLF is one cursor step.
class EditBuffer {
public:
    Document document;
    std::string baseline;
    bool bom = false;
    bool saving = false;
    uint64_t revision = 0;
    bool dirty() const { return document.text != savedText_; }
    void initialize(std::string bytes);
    void saved(std::string bytes, std::string text);
    bool replace(size_t begin, size_t end, std::string text);
    bool undo(size_t& anchor, size_t& caret);
    bool redo(size_t& anchor, size_t& caret);
    std::string encoded() const;
    std::string newline() const;
    size_t previous(size_t offset) const;
    size_t next(size_t offset) const;
private:
    struct Change { size_t begin; std::string before, after; };
    std::string savedText_;
    std::deque<Change> undo_, redo_;
    size_t historyBytes_ = 0;
};
}
