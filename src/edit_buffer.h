#pragma once
#include "text.h"
#include <deque>

namespace poppy {
// Byte offsets always point to UTF-8 boundaries; CRLF is one cursor step.
class EditBuffer {
public:
    Document document;
    std::string baseline;
    std::string fileVersion;
    bool bom = false;
    bool saving = false;
    uint64_t revision = 0;
    bool dirty() const { return document.text != savedText_; }
    void initialize(std::string bytes);
    void saved(std::string bytes, std::string text);
    bool replace(size_t begin, size_t end, std::string text);
    // With no selection, cut the logical line, including its following newline.
    // At EOF, remove the preceding newline without copying it.
    struct CutRange { size_t begin, end; std::string text; };
    CutRange cutRange(size_t anchor, size_t caret) const;
    bool moveLines(size_t& anchor, size_t& caret, bool down);
    bool undo(size_t& anchor, size_t& caret);
    bool redo(size_t& anchor, size_t& caret);
    std::string encoded() const;
    std::string newline() const;
    size_t previous(size_t offset) const;
    size_t next(size_t offset) const;
    size_t previousWord(size_t offset) const;
    size_t nextWord(size_t offset) const;
private:
    struct Change { size_t begin; std::string before, after; };
    std::string savedText_;
    std::deque<Change> undo_, redo_;
    size_t historyBytes_ = 0;
};
}
