#include "terminal_model.h"
#include "text.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

using namespace poppy;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::string line(const TerminalModel& terminal, int y, size_t scroll = 0) {
    std::string result;
    for (const auto& cell : terminal.row(y, scroll)) if (!cell.continuation) appendUtf8(result, cell.ch);
    while (!result.empty() && result.back() == ' ') result.pop_back();
    return result;
}
void textTests() {
    std::string decoded, encoding, error;
    check(decodeText("\xef\xbb\xbfhello\r\n世界", decoded, encoding, error), "UTF-8 BOM accepted");
    check(decoded == "hello\r\n世界" && encoding == "UTF-8", "UTF-8 BOM removed");
    const std::string utf16{char(0xff), char(0xfe), char(0x42), char(0x30), char(0x3d), char(0xd8), char(0x00), char(0xde)};
    check(decodeText(utf16, decoded, encoding, error) && decoded == "あ😀", "UTF-16 surrogate pair converted");
    const std::string bigEndian{char(0xfe), char(0xff), char(0x30), char(0x42)};
    check(decodeText(bigEndian, decoded, encoding, error) && decoded == "あ", "UTF-16 big endian converted");
    check(!decodeText(std::string("a\0b", 3), decoded, encoding, error), "binary rejected");
    check(!decodeText("\xc0\xaf", decoded, encoding, error), "overlong UTF-8 rejected");
    check(!decodeText("\xed\xa0\x80", decoded, encoding, error), "UTF-8 surrogate rejected");
    check(!decodeText("\xe3\x81", decoded, encoding, error), "truncated UTF-8 rejected");
    const std::string brokenSurrogate{char(0xff), char(0xfe), char(0x00), char(0xdc)};
    check(!decodeText(brokenSurrogate, decoded, encoding, error), "unpaired UTF-16 surrogate rejected");
    check(decodeText("\xef\xbf\xbd", decoded, encoding, error), "literal replacement character is valid UTF-8");
    size_t p = 0; decodeUtf8("\xe3\x81", p, false); check(p == 0, "incomplete streaming scalar retained");
    p = 0; decodeUtf8("\xe3\x1b", p, false); check(p == 1, "invalid streaming scalar does not swallow ESC");

    Document document; document.text = "/* comment\r\nstill comment */ int value = 42;\n"; document.language = Language::CLike;
    indexDocument(document);
    check(document.lines.size() == 3 && document.line(0) == "/* comment" && document.line(2).empty(), "CRLF and trailing empty line indexed");
    check(document.states[1] == LexState::BlockComment && document.states[2] == LexState::Normal, "multiline lexer state indexed");
    LexState state = document.states[1]; std::vector<Token> tokens;
    lexLine(document.line(1), document.language, state, &tokens);
    check(!tokens.empty() && tokens[0].style == Style::Comment && state == LexState::Normal, "visible line starts in its stored lexical state");
    bool keyword = false, number = false;
    for (const auto& token : tokens) { keyword |= token.style == Style::Keyword; number |= token.style == Style::Number; }
    check(keyword && number, "tokens after closing comment recognized");
    state = LexState::Normal; lexLine("x = '''hello", Language::Python, state); check(state == LexState::TripleSingle, "Python triple string opens");
    lexLine("world''' # end", Language::Python, state, &tokens); check(state == LexState::Normal && tokens.back().style == Style::Comment, "Python triple string closes");
    state = LexState::Normal; lexLine("`hello", Language::CLike, state, &tokens); check(state == LexState::Backtick && tokens.front().begin == 0, "template literal delimiter highlighted");
    check(cellWidth(U'あ') == 2 && cellWidth(U'A') == 1 && cellWidth(U'\u0301') == 0, "CJK and combining widths");
    check(languageFor(".TSX") == Language::CLike && languageFor(".unknown") == Language::Plain, "language detection");
    state = LexState::Normal; lexLine("-old", Language::Diff, state, &tokens); check(tokens[0].style == Style::Removed, "diff removal highlighted");
}
void lineIndexTests() {
    // Put a wide glyph across a checkpoint, then mix UTF-8, tabs, surrogate
    // pairs and combining marks throughout a line longer than the display cap.
    std::string text(63, 'x'); text += "日\t";
    while (text.size() < maxLineColumns * 4) text += "ab日\t🙂e\xcc\x81";
    LineIndex index; index.build(text, Language::Plain, LexState::Normal);
    const auto boundary = index.before(64);
    check(boundary.column == 63 && boundary.byte == 63, "checkpoint keeps the leading byte of a wide glyph");
    std::vector<LineIndex::Position> expected(maxLineColumns + 1);
    uint32_t column = 0;
    for (size_t p = 0; p < text.size() && column < maxLineColumns;) {
        const LineIndex::Position start{static_cast<uint32_t>(p), column};
        const auto c = decodeUtf8(text, p);
        const auto width = c == '\t' ? 4 - column % 4 : std::max(1, cellWidth(c));
        for (int i = 0; i < width && column + i <= maxLineColumns; ++i) expected[column + i] = start;
        column += width;
        if (column <= maxLineColumns) expected[column] = {static_cast<uint32_t>(p), column};
    }
    for (uint32_t target = 0; target <= maxLineColumns; ++target) {
        const auto start = index.before(target);
        check(start.column <= target && target - start.column <= LineIndex::stride + 3,
            "horizontal scrolling decoded an unbounded line prefix");
        size_t byte = start.byte; auto col = start.column;
        while (col < target) {
            const auto before = byte;
            const auto c = decodeUtf8(text, byte);
            const auto width = c == '\t' ? 4 - col % 4 : std::max(1, cellWidth(c));
            if (col + width > target) { byte = before; break; }
            col += width;
        }
        check(byte == expected[target].byte && col == expected[target].column,
            "scrolling from a checkpoint changed a UTF-8 byte or cell position");
    }
    check(index.bytes() < 4096, "plain long-line checkpoints exceeded their sparse memory budget");
    check(index.before(1000000).column <= maxLineColumns, "checkpoints exceed the long-line display cap");
    index.build(std::string(5000, 'x') + " */ int n = 42;", Language::CLike, LexState::BlockComment);
    bool keyword = false, number = false;
    for (const auto& token : index.tokens()) { keyword |= token.style == Style::Keyword; number |= token.style == Style::Number; }
    check(index.tokens().front().style == Style::Comment && keyword && number,
        "cached tokens lost a multiline comment or the code after it");
    index.build("", Language::Plain, LexState::Normal);
    check(index.before(8192).byte == 0 && index.before(8192).column == 0 && index.tokens().empty(),
        "rebuilding an empty line retained stale positions or styles");
    index.build("a\tb", Language::Plain, LexState::Normal);
    check(index.before(8192).byte == 0, "a short line produced an invalid checkpoint");
}
void terminalTests() {
    TerminalModel terminal(8, 3);
    terminal.feed("hello\rX"); check(line(terminal, 0) == "Xello", "CR overwrites without erasing the row");
    terminal.feed("\x1b[2J\x1b[H"); check(line(terminal, 0).empty(), "erase screen and home");
    terminal.feed("\xe3\x81"); check(terminal.cursorX() == 0, "split UTF-8 waits for final byte");
    terminal.feed("\x82!"); check(line(terminal, 0) == "あ!" && terminal.cursorX() == 3, "split CJK occupies two cells");
    check(terminal.row(0)[1].continuation, "wide continuation recorded");
    terminal.feed("\r\x1b[2GX"); check(terminal.row(0)[0].ch == U' ' && !terminal.row(0)[1].continuation, "overwriting wide continuation clears its lead");

    TerminalModel colors(8, 2);
    colors.feed("\x1b[38;2;1;2"); colors.feed(";3mX\x1b[0mY");
    check(colors.row(0)[0].fg == rgb(1,2,3) && colors.row(0)[1].fg == foreground, "split CSI truecolor and reset");
    colors.feed("\x1b[48;5;196mZ"); check(colors.row(0)[2].bg == rgb(255,0,0), "256 color background");

    TerminalModel wrap(4, 2); wrap.feed("abcd"); check(wrap.cursorY() == 0, "wrap is delayed until next printable character");
    wrap.feed("e"); check(line(wrap, 0) == "abcd" && line(wrap, 1) == "e", "delayed wrapping advances a row");
    wrap.feed("\x1b[?7l\x1b[2;4HXY"); check(line(wrap, 1) == "e  Y", "disabled wrapping overwrites last column");

    TerminalModel scroll(8, 2); scroll.feed("one\r\ntwo\r\nthree");
    check(scroll.historySize() == 1 && line(scroll, 0) == "two" && line(scroll, 0, 1) == "one", "scrollback retains previous row");
    scroll.feed("\x1b[?1049hALT\x1b[?2004h");
    check(scroll.alternate() && line(scroll, 0) == "ALT" && scroll.historySize() == 0 && scroll.bracketedPaste(), "alternate buffer and bracketed paste");
    scroll.feed("\x1b[?1049l"); check(!scroll.alternate() && line(scroll, 1) == "three" && scroll.cursorX() == 5, "normal screen and cursor restored");
    scroll.feed("\x1b[3J"); check(scroll.historySize() == 0, "explicit history erase");

    TerminalModel region(6, 4); region.feed("A\r\nB\r\nC\r\nD\x1b[2;3r\x1b[3;1H\n");
    check(line(region, 0) == "A" && line(region, 1) == "C" && line(region, 2).empty() && line(region, 3) == "D" && region.historySize() == 0, "scroll region preserves surrounding rows");
    region.feed("\x1b[?6h\x1b[1;1HX"); check(line(region, 1) == "X", "origin mode addresses relative to scroll region");

    TerminalModel query(12, 3); std::string response;
    query.reply = [&](std::string bytes) { response += bytes; };
    query.feed("\x1b[2;3HX\x1b[6n"); check(response == "\x1b[2;4R", "cursor position query");
    response.clear(); query.feed("\x1b]11;?\x1b"); query.feed("\\"); check(response == "\x1b]11;rgb:1616/1919/1f1f\x1b\\", "split OSC theme query");
    response.clear(); query.feed("\x1b]52;c;aGVsbG8=\a"); check(response.empty(), "OSC clipboard request has no side effects");
    query.feed("\x1bPignored\x1b\\Z"); check(line(query, 1) == "  XZ", "DCS ignored until string terminator");
    query.feed("\x1b[" + std::string(1024, '1') + "mQ"); check(line(query, 1) == "  XZQ", "oversized CSI discarded and parser recovers");

    TerminalModel edits(8, 2); edits.feed("abcdef\r\x1b[3G\x1b[2P"); check(line(edits, 0) == "abef", "delete characters shifts row");
    edits.feed("\x1b[2@XY"); check(line(edits, 0) == "abXYef", "insert characters shifts row");
    edits.feed("\x1b" "7\x1b[2;1HZ\x1b" "8"); check(edits.cursorY() == 0 && edits.cursorX() == 4, "save and restore cursor");

    TerminalModel resized(8, 4); resized.feed("A\r\nB\r\nC\r\nD"); resized.resize(4, 2);
    check(resized.cursorY() == 1 && line(resized, 0) == "C" && line(resized, 1) == "D", "shrinking pane keeps cursor visible");
    TerminalModel wideEnd(4, 2); wideEnd.feed("abあ"); wideEnd.resize(4, 3);
    check(wideEnd.row(0)[2].ch == U'あ' && wideEnd.row(0)[3].continuation, "resize keeps a complete wide glyph at right edge");
    wideEnd.resize(3, 3); check(wideEnd.row(0)[2].ch == U' ', "resize removes clipped wide glyph");

    TerminalModel bounded(500, 2);
    for (int i = 0; i < 2000; ++i) bounded.feed("x\r\n");
    check(bounded.historySize() <= 131072 / 500, "history cell budget enforced");
    bounded.resize(8, 2); for (int i = 0; i < 2000; ++i) bounded.feed("x\r\n");
    check(bounded.historySize() <= 1000, "history line budget enforced across resizes");
    check(sizeof(Cell) <= 16, "cell memory budget");
}
}
int main() {
    try { textTests(); lineIndexTests(); terminalTests(); std::cout << "Text and terminal core tests passed.\n"; return EXIT_SUCCESS; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return EXIT_FAILURE; }
}
