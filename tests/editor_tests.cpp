#include "edit_buffer.h"
#include <cstdlib>
#include <iostream>
using namespace poppy;
void check(bool value) { if (!value) { std::cerr << "Editor check failed\n"; std::exit(1); } }
EditBuffer load(std::string bytes) {
    EditBuffer buffer; std::string error;
    check(decodeText(bytes, buffer.document.text, buffer.document.encoding, error));
    indexDocument(buffer.document); buffer.initialize(std::move(bytes)); return buffer;
}
int main() {
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
    std::cout << "Editor encoding, selection, Unicode, history and limits passed\n";
}
