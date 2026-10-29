#include "edit_buffer.h"
#include "text_layout.h"
#include <cstdlib>
#include <iostream>
using namespace poppy;
void check(bool value) { if (!value) { std::cerr << "Editor check failed\n"; std::exit(1); } }
EditBuffer load(std::string bytes) {
    EditBuffer buffer; std::string error;
    check(decodeText(bytes, buffer.document.text, buffer.document.encoding, error));
    indexDocument(buffer.document); buffer.initialize(std::move(bytes)); return buffer;
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
int main() {
    wordMovement();
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
    std::cout << "Editor encoding, selection, word movement, Unicode, history, wrapping and limits passed\n";
}
