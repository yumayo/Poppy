#include "edit_buffer.h"
#include "text_layout.h"
#include "selection_scroll.h"
#include "composition.h"
#include "terminal_model.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
using namespace poppy;
void check(bool value) { if (!value) { std::cerr << "Editor check failed\n"; std::exit(1); } }
void compositionLayout() {
    CompositionText text{u"iWいあ日ｱ😀Z", {0, 0, 1, 1, 2, 0, 3, 3, 0}, 0};
    const std::vector<int> cursorColumns{0, 1, 2, 4, 6, 8, 9, 9, 11, 12};
    for (size_t cursor = 0; cursor < cursorColumns.size(); ++cursor) {
        text.cursor = cursor;
        const auto layout = text.layout(32, 0);
        check(layout.endRow == 0 && layout.endColumn == 12);
        check(layout.glyphs.size() == 8 && layout.caretRow == 0 && layout.caretColumn == cursorColumns[cursor]);
        check(layout.glyphs[2].character == U'い' && layout.glyphs[2].width == 2 && layout.glyphs[2].attribute == 1);
        check(layout.glyphs[5].character == U'ｱ' && layout.glyphs[5].width == 1);
        check(layout.glyphs[6].character == U'😀' && layout.glyphs[6].column == 9 && layout.glyphs[6].attribute == 3);
        // Natural advances of i/W or Japanese kana must never affect placement.
        TerminalModel terminal(32, 2); terminal.feed(u8"iWいあ日ｱ😀Z");
        for (const auto& glyph : layout.glyphs) {
            check(terminal.row(glyph.row)[glyph.column].ch == glyph.character);
            if (glyph.width == 2) check(terminal.row(glyph.row)[glyph.column + 1].continuation);
        }
    }
    text = {u"日AあB", {1, 2, 3, 4}, 4};
    auto layout = text.layout(6, 5);
    check(layout.glyphs[0].row == 1 && layout.glyphs[0].column == 0 && layout.glyphs[0].width == 2);
    check(layout.glyphs[1].row == 1 && layout.glyphs[1].column == 2);
    check(layout.glyphs[2].row == 1 && layout.glyphs[2].column == 3 && layout.glyphs[2].attribute == 3);
    check(layout.caretRow == 2 && layout.caretColumn == 0);
    text.cursor = 0; layout = text.layout(6, 5);
    check(layout.caretRow == 1 && layout.caretColumn == 0);
    text = {std::u16string{0xd800, u'A', 0xdc00}, {}, 999};
    layout = text.layout(8, 0);
    check(layout.glyphs.size() == 3 && layout.glyphs[0].character == 0xfffd && layout.glyphs[2].character == 0xfffd);
    check(layout.caretRow == 0 && layout.caretColumn == 3 && layout.glyphs[2].attribute == 0);
    for (const int columns : {0, 1, 2, 3, 5, 80}) {
        text = {u"あいう😀日A", {}, 7};
        layout = text.layout(columns, columns - 1);
        for (const auto& glyph : layout.glyphs) check(glyph.column >= 0 && glyph.column + glyph.width <= std::max(2, columns));
    }
    text = {}; layout = text.layout(8, 5);
    check(layout.glyphs.empty() && layout.caretRow == 0 && layout.caretColumn == 5);
    text = {u"A", {}, 0}; layout = text.layout(8, 8);
    check(layout.glyphs[0].row == 1 && layout.glyphs[0].column == 0 && layout.caretRow == 1 && layout.caretColumn == 0);
}
EditBuffer load(std::string bytes) {
    EditBuffer buffer; std::string error;
    check(decodeText(bytes, buffer.document.text, buffer.document.encoding, error));
    indexDocument(buffer.document); buffer.initialize(std::move(bytes)); return buffer;
}
void lineCutting() {
    struct Case { std::string before; size_t anchor, caret, begin, end; std::string copied, after; };
    const std::vector<Case> cases{
        {"one\ntwo\nthree", 1, 1, 0, 4, "one\n", "two\nthree"},
        {"one\ntwo\nthree", 5, 5, 4, 8, "two\n", "one\nthree"},
        {"one\ntwo\nthree", 8, 8, 7, 13, "three", "one\ntwo"},
        {"one\ntwo\nthree", 13, 13, 7, 13, "three", "one\ntwo"},
        {"one\r\ntwo\r\nthree", 6, 6, 5, 10, "two\r\n", "one\r\nthree"},
        {"one\r\ntwo\r\nthree", 15, 15, 8, 15, "three", "one\r\ntwo"},
        {"one\n\ntwo", 4, 4, 4, 5, "\n", "one\ntwo"},
        {"one\r\n\r\ntwo", 5, 5, 5, 7, "\r\n", "one\r\ntwo"},
        {"one\n", 4, 4, 3, 4, "", "one"},
        {"one\r\n", 5, 5, 3, 5, "", "one"},
        {"only", 2, 2, 0, 4, "only", ""},
        {"", 0, 0, 0, 0, "", ""},
        {"one\ntwo", 1, 5, 1, 5, "ne\nt", "owo"},
        {"one\ntwo", 5, 1, 1, 5, "ne\nt", "owo"},
        {u8"日本😀\r\n最後", 3, 3, 0, 12, u8"日本😀\r\n", u8"最後"},
        {std::string(2000, 'x') + "\ntail", 1000, 1000, 0, 2001, std::string(2000, 'x') + "\n", "tail"}
    };
    for (const auto& test : cases) {
        auto buffer = load(test.before);
        const auto range = buffer.cutRange(test.anchor, test.caret);
        check(range.begin == test.begin && range.end == test.end && range.text == test.copied);
        check(buffer.replace(range.begin, range.end, "") && buffer.document.text == test.after);
        if (test.begin == test.end) { check(buffer.revision == 0 && !buffer.dirty()); continue; }
        size_t anchor = 0, caret = 0;
        check(buffer.undo(anchor, caret) && buffer.encoded() == test.before && !buffer.dirty());
        check(!buffer.undo(anchor, caret));
        check(buffer.redo(anchor, caret) && buffer.document.text == test.after);
        check(buffer.document.lines == load(test.after).document.lines);
    }
}
void lineMovement() {
    struct Case { std::string before; size_t anchor, caret; bool down; std::string after; size_t movedAnchor, movedCaret; };
    const std::vector<Case> cases{
        {"a\nbb\nccc", 3, 3, false, "bb\na\nccc", 1, 1},
        {"a\nbb\nccc", 3, 3, true, "a\nccc\nbb", 7, 7},
        {"a\nbb\nccc", 7, 7, false, "a\nccc\nbb", 4, 4},
        {"a\r\nbb\r\nccc", 4, 4, true, "a\r\nccc\r\nbb", 9, 9},
        {"a\r\nbb\r\nccc", 9, 9, false, "a\r\nccc\r\nbb", 5, 5},
        {"a\nbb\nccc\n", 3, 3, true, "a\nccc\nbb\n", 7, 7},
        {"a\n", 2, 2, false, "\na", 0, 0},
        {"a\n", 0, 0, true, "\na", 1, 1},
        {"a\r\n", 3, 3, false, "\r\na", 0, 0},
        {"a\n\nb", 2, 2, true, "a\nb\n", 4, 4},
        {"a\nbb\nccc\ndddd", 3, 7, false, "bb\nccc\na\ndddd", 1, 5},
        {"a\nbb\nccc\ndddd", 7, 3, false, "bb\nccc\na\ndddd", 5, 1},
        {"a\nbb\nccc\ndddd", 3, 9, true, "a\ndddd\nbb\nccc", 8, 13},
        {"a\nbb\nccc\ndddd", 9, 3, true, "a\ndddd\nbb\nccc", 13, 8},
        {"a\nbb\nccc\ndddd", 2, 5, false, "bb\na\nccc\ndddd", 0, 3},
        {"a\nbb\nccc\ndddd", 5, 2, false, "bb\na\nccc\ndddd", 3, 0},
        {"a\r\nbb\nccc\r\nd", 4, 4, true, "a\r\nccc\nbb\r\nd", 8, 8},
        {"a\r\nbb\nccc\r\nd", 4, 8, false, "bb\r\nccc\na\r\nd", 1, 6},
        {u8"日本\r\n😀abc\r\n最後", 12, 12, true, u8"日本\r\n最後\r\n😀abc", 20, 20},
        {std::string(2000, 'x') + "\nend", 1000, 1000, true, "end\n" + std::string(2000, 'x'), 1004, 1004}
    };
    for (const auto& test : cases) {
        auto buffer = load(test.before);
        size_t anchor = test.anchor, caret = test.caret;
        check(buffer.moveLines(anchor, caret, test.down));
        if (buffer.document.text != test.after || anchor != test.movedAnchor || caret != test.movedCaret) {
            std::cerr << "Line movement failed at " << test.anchor << ',' << test.caret << " down=" << test.down
                << ": got " << anchor << ',' << caret << " expected " << test.movedAnchor << ',' << test.movedCaret << '\n';
            check(false);
        }
        check(buffer.document.lines == load(test.after).document.lines && buffer.revision == 1);
        check(buffer.undo(anchor, caret) && buffer.encoded() == test.before && !buffer.dirty());
        check(!buffer.undo(anchor, caret));
        check(buffer.redo(anchor, caret) && buffer.document.text == test.after);
    }
    for (const auto& text : {"", "only", "a\nb\n", "a\r\nb"}) {
        auto buffer = load(text);
        size_t anchor = 0, caret = 0;
        check(!buffer.moveLines(anchor, caret, false) && anchor == 0 && caret == 0);
        anchor = caret = buffer.document.text.size();
        check(!buffer.moveLines(anchor, caret, true) && anchor == caret && caret == buffer.document.text.size());
        anchor = 0;
        check(!buffer.moveLines(anchor, caret, false) && anchor == 0 && caret == buffer.document.text.size());
        if (buffer.document.text.empty() || buffer.document.text.back() != '\n')
            check(!buffer.moveLines(anchor, caret, true) && anchor == 0 && caret == buffer.document.text.size());
        check(!buffer.dirty() && buffer.revision == 0 && !buffer.undo(anchor, caret));
    }
    for (const auto& bytes : {std::string("\xef\xbb\xbf") + u8"日本\r\n😀", std::string("\xff\xfe" "a\0\r\0\n\0b\0", 10), std::string("\xfe\xff\0a\0\n\0b", 8)}) {
        auto buffer = load(bytes); const auto original = buffer.document.text;
        size_t anchor = 0, caret = 0;
        check(buffer.moveLines(anchor, caret, true));
        check(load(buffer.encoded()).document.text == buffer.document.text);
        check(buffer.undo(anchor, caret) && buffer.encoded() == bytes && buffer.document.text == original);
    }
}
void wordMovement() {
    auto buffer = load("foo_bar42  += bar.baz\r\n\r\n  end \t");
    const std::vector<size_t> stops{0, 11, 14, 17, 18, 27, 32};
    for (size_t i = 1; i < stops.size(); ++i) {
        check(buffer.nextWord(stops[i - 1]) == stops[i]);
        check(buffer.previousWord(stops[i]) == stops[i - 1]);
    }
    check(buffer.previousWord(0) == 0 && buffer.nextWord(32) == 32);
    check(buffer.nextWord(3) == 11 && buffer.previousWord(3) == 0);
    check(buffer.nextWord(10) == 11 && buffer.previousWord(10) == 0);
    check(buffer.nextWord(12) == 14 && buffer.previousWord(12) == 11);
    check(buffer.nextWord(21) == 27 && buffer.previousWord(23) == 18);
    check(buffer.nextWord(100) == 32 && buffer.previousWord(100) == 27);

    buffer = load(u8"日本語　café_42、😀 next\r\n最後");
    const std::vector<size_t> unicodeStops{0, 12, 20, 28, 34, 40};
    for (size_t i = 1; i < unicodeStops.size(); ++i) {
        check(buffer.nextWord(unicodeStops[i - 1]) == unicodeStops[i]);
        check(buffer.previousWord(unicodeStops[i]) == unicodeStops[i - 1]);
    }
    check(buffer.nextWord(3) == 12 && buffer.previousWord(6) == 0);
    check(buffer.nextWord(23) == 28 && buffer.previousWord(27) == 20);
    for (size_t p = 0; p < buffer.document.text.size(); p = buffer.next(p)) {
        const auto right = buffer.nextWord(p), left = buffer.previousWord(p);
        check(right > p && (left < p || p == 0));
        check(right <= buffer.document.text.size());
        for (const auto offset : {left, right}) {
            check(offset == buffer.document.text.size() || (static_cast<unsigned char>(buffer.document.text[offset]) & 0xc0) != 0x80);
            check(!offset || buffer.document.text[offset - 1] != '\r');
        }
    }
    check(!buffer.dirty() && buffer.encoded() == u8"日本語　café_42、😀 next\r\n最後");
    buffer = load(u8"a\u00a0b\u2009c\u3000d");
    check(buffer.nextWord(0) == 3 && buffer.nextWord(3) == 7 && buffer.nextWord(7) == 11);
    check(buffer.previousWord(11) == 7 && buffer.previousWord(7) == 3 && buffer.previousWord(3) == 0);
    buffer = load(u8"naïve e\u0301clair");
    check(buffer.nextWord(0) == 7 && buffer.nextWord(7) == buffer.document.text.size());
    check(buffer.previousWord(buffer.document.text.size()) == 7);
    for (const auto& text : {std::string{}, std::string(" \t\r\n\n  "), std::string(10000, 'x')}) {
        buffer = load(text);
        check(buffer.nextWord(0) == text.size() && buffer.previousWord(text.size()) == 0);
        check(buffer.previousWord(0) == 0 && buffer.nextWord(text.size()) == text.size());
    }
}
void wordSelection() {
    const std::vector<std::vector<std::string>> cases{
        {"foo_bar42", "  ", "+=", " ", "bar", ".", "baz"},
        {u8"日本語", u8"　", u8"café_42", u8"、😀", " ", "next"},
        {u8"naïve", "\t ", u8"e\u0301clair", u8"\u00a0\u2009", "end"},
        {"\t  "}, {std::string(10000, 'x')}, {}
    };
    for (const auto& tokens : cases) {
        std::string text;
        for (const auto& token : tokens) text += token;
        auto buffer = load(text);
        size_t begin = 0;
        for (const auto& token : tokens) {
            for (size_t p = begin; p < begin + token.size(); ++p) {
                const auto range = wordRange(buffer.document.line(0), p);
                check(range.begin == begin && range.end == begin + token.size());
                check(text.substr(range.begin, range.end - range.begin) == token);
            }
            begin += token.size();
        }
        for (const auto p : {text.size(), text.size() + 100}) {
            const auto range = wordRange(buffer.document.line(0), p);
            check(range.begin == text.size() && range.end == text.size());
        }
        check(!buffer.dirty() && buffer.revision == 0 && buffer.encoded() == text);
    }
    for (const auto& newline : {"\n", "\r\n"}) {
        const auto buffer = load(std::string("first") + newline + newline + "last");
        for (size_t row = 0; row < buffer.document.lines.size(); ++row) {
            const auto line = buffer.document.line(row);
            const auto range = wordRange(line, 0);
            check(range.begin == 0 && range.end == line.size());
            const auto end = wordRange(line, line.size());
            check(end.begin == line.size() && end.end == line.size());
        }
    }
}
void wrapping() {
    auto buffer = load("abcdefgh\r\n\r\nxyz\r\n");
    const auto original = buffer.encoded();
    TextLayout layout; layout.build(buffer.document, 4);
    check(layout.rows() == 5);
    auto row = layout.row(buffer.document, 1);
    check(row.line == 0 && row.begin == 4 && row.end == 8 && row.column == 4);
    check(layout.rowForOffset(buffer.document, 4) == 1);
    check(layout.rowForOffset(buffer.document, 4, true) == 0);
    check(layout.rowForOffset(buffer.document, 8) == 1);
    check(layout.row(buffer.document, 2).begin == layout.row(buffer.document, 2).end);
    check(layout.row(buffer.document, 4).line == 3);
    layout.build(buffer.document, 8); check(layout.rows() == 4);
    check(buffer.encoded() == original && !buffer.dirty());

    buffer = load(u8"abc日本😀z\na\tbc\td"); layout.build(buffer.document, 4);
    row = layout.row(buffer.document, 1);
    check(row.begin == 3 && row.end == 9 && row.column == 3);
    row = layout.row(buffer.document, 2);
    check(row.begin == 9 && row.end == 14 && row.column == 7);
    row = layout.row(buffer.document, 4);
    check(row.line == 1 && row.begin == 2 && row.end == 5 && row.column == 4);
    // A narrow viewport must still consume whole Unicode characters and tabs.
    for (const int columns : {0, 1, 2, 3, 4, 5, 9, 80}) {
        layout.build(buffer.document, columns);
        size_t previousLine = 0, previousEnd = 0;
        for (size_t i = 0; i < layout.rows(); ++i) {
            row = layout.row(buffer.document, i);
            if (row.line != previousLine) previousEnd = 0;
            check(row.begin == previousEnd && row.end > row.begin);
            const size_t offset = buffer.document.lines[row.line] + row.begin;
            check(layout.rowForOffset(buffer.document, offset) == i);
            check(layout.rowForOffset(buffer.document, buffer.document.lines[row.line] + row.end, true) == i);
            check((static_cast<unsigned char>(buffer.document.text[offset]) & 0xc0) != 0x80);
            previousLine = row.line; previousEnd = row.end;
        }
    }
    buffer = load(""); layout.build(buffer.document, 4); check(layout.rows() == 1);
    buffer = load(std::string(maxLineColumns + 100, 'x')); layout.build(buffer.document, 1);
    check(layout.rows() == maxLineColumns);
    check(layout.row(buffer.document, layout.rows() - 1).end == maxLineColumns);
    check(layout.rowForOffset(buffer.document, buffer.document.text.size()) == layout.rows() - 1);
}
void selectionScrolling() {
    for (const int height : {20, 30, 40}) {
        // Headers, search bars and DPI change the text viewport, not direction.
        const int top = 3 * height, bottom = top + 10 * height;
        for (int y = top; y < bottom; ++y) check(selectionScrollRows(y, top, bottom, height) == 0);
        check(selectionScrollRows(top - 1, top, bottom, height) == -1);
        check(selectionScrollRows(bottom, top, bottom, height) == 1);
        check(selectionScrollRows(top - height, top, bottom, height) == -1);
        check(selectionScrollRows(bottom + height - 1, top, bottom, height) == 1);
        check(selectionScrollRows(top - height - 1, top, bottom, height) == -2);
        check(selectionScrollRows(bottom + height, top, bottom, height) == 2);
        check(selectionScrollRows(-32000, top, bottom, height) == -8);
        check(selectionScrollRows(32000, top, bottom, height) == 8);
    }
    check(selectionScrollRows(-10, 40, 40, 20) == 0);
    check(selectionScrollRows(100, 40, 20, 20) == 0);
    check(selectionScrollRows(100, 0, 80, 0) == 0);
}
int main() {
    compositionLayout();
    lineCutting();
    lineMovement();
    selectionScrolling();
    wordMovement();
    wordSelection();
    wrapping();
    for (const auto& bytes : {std::string("hello\r\nworld\r\n"), std::string("\xef\xbb\xbf") + u8"日本語\n😀", std::string("\xff\xfeh\0i\0\r\0\n\0", 10), std::string("\xfe\xff\0h\0i\0\n", 8)}) {
        auto buffer = load(bytes); check(!buffer.dirty()); check(buffer.encoded() == bytes);
        const auto original = buffer.document.text;
        check(buffer.replace(0, 0, u8"修正😀")); check(buffer.dirty());
        size_t anchor = 0, caret = 0; check(buffer.undo(anchor, caret)); check(!buffer.dirty()); check(buffer.encoded() == bytes);
        check(buffer.redo(anchor, caret)); check(buffer.dirty());
        auto edited = buffer.encoded(); auto again = load(edited); check(again.document.text == u8"修正😀" + original);
        buffer.saved(edited, buffer.document.text); check(!buffer.dirty()); check(buffer.undo(anchor, caret)); check(buffer.dirty());
    }
    auto buffer = load(u8"a😀日本\r\nz");
    check(buffer.next(1) == 5); check(buffer.previous(5) == 1);
    const auto newline = buffer.document.text.find('\r'); check(buffer.next(newline) == newline + 2); check(buffer.previous(newline + 2) == newline);
    check(!buffer.replace(2, 3, "x")); check(!buffer.replace(0, 0, std::string(1, '\0'))); check(!buffer.replace(0, 0, "\xff"));
    check(!buffer.replace(0, 0, std::string(maxFileBytes, 'x')));
    check(buffer.replace(1, 5, "X")); check(buffer.document.text == u8"aX日本\r\nz");
    check(buffer.newline() == "\r\n"); check(buffer.document.lines.size() == 2);
    std::cout << "Editor encoding, selection, word movement/selection, Unicode, history, wrapping and limits passed\n";
}
