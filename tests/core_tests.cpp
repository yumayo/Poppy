#include "terminal_model.h"
#include "terminal_display.h"
#include "text.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>

using namespace poppy;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class Terminal> std::string line(const Terminal& terminal, int y) {
    std::string result;
    for (const auto& cell : terminal.row(y)) if (!cell.continuation) appendUtf8(result, cell.ch);
    while (!result.empty() && result.back() == ' ') result.pop_back();
    return result;
}
std::string line(const TerminalModel& terminal, int y, size_t scroll) {
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
void terminalDisplayTests() {
    TerminalModel query(4, 2); std::string response;
    query.reply = [&](std::string bytes) { response += bytes; };
    query.feed("abcd\x1b[?2026$"); query.feed("p");
    check(response == "\x1b[?2026;2$y", "synchronized output query reports supported and reset");
    response.clear(); query.feed("\x1b[?2026"); query.feed("h\x1b[?2026$p");
    check(query.synchronizedOutput() && response == "\x1b[?2026;1$y", "split begin marker and active mode query");
    query.feed("\x1b[?2026lE");
    check(!query.synchronizedOutput() && line(query, 0) == "abcd" && line(query, 1) == "E", "sync controls and queries preserve delayed wrapping");
    query.feed("\x1b[?2026h\x1b" "c"); check(!query.synchronizedOutput(), "terminal reset releases synchronized output");

    TerminalModel model(12, 3); TerminalDisplay display;
    model.feed("old"); display.update(model, 0, 0, false, true);
    model.feed("\x1b[?2026h\x1b[H\x1b[2J\x1b[31m途中\x1b[?25l"); display.changed(10);
    check(!display.update(model, 0, 10) && display.pending(), "begin marker defers presentation");
    check(!display.update(model, 0, 500) && line(display, 0) == "old" && display.cursorX() == 3 && display.cursorVisible(), "incidental paints retain committed text and cursor");
    model.feed("\x1b[H\x1b[2J完成\x1b[?25h\x1b[?2026"); display.changed(501);
    check(!display.update(model, 0, 501) && line(display, 0) == "old", "split end marker does not expose intermediate text");
    model.feed("l"); display.changed(502);
    check(display.update(model, 0, 502) && line(display, 0) == "完成" && display.cursorX() == 4 && !display.pending(), "completed frame is presented with its cursor");
    check(display.row(0)[0].fg == model.row(0)[0].fg && display.row(0)[1].continuation, "committed frame retains colors and wide cells");

    // Simulate resume output split into the same 8 KiB chunks used by the UI.
    std::string replay = "\x1b[?2026h";
    for (int i = 0; i < 4000; ++i) replay += "history entry\r\n";
    replay += "\x1b[H\x1b[2Jready\x1b[?2026l";
    uint64_t tick = 600;
    for (size_t offset = 0; offset < replay.size(); offset += 8192) {
        model.feed(std::string_view(replay).substr(offset, 8192)); display.changed(tick);
        const bool last = offset + 8192 >= replay.size();
        check(display.update(model, 0, tick, !last) == last, "replayed history is only presented after its end marker");
        check(line(display, 0) == (last ? "ready" : "完成"), "resume does not flash replayed history");
        ++tick;
    }

    model.feed("\x1b[?2026h\rtimeout"); display.changed(1000);
    check(!display.update(model, 0, 1000), "incomplete synchronized output waits");
    model.feed("\x1b[?2026h"); display.changed(1900);
    check(!display.update(model, 0, 1999), "repeated begin marker does not restart the timeout");
    check(display.update(model, 0, 2000) && !model.synchronizedOutput() && !display.pending(), "missing end marker is released after one second without more output");
    model.feed("\x1b[?2026h\rended"); display.changed(2010); display.update(model, 0, 2010);
    check(display.update(model, 0, 2011, false, true) && !model.synchronizedOutput() && line(display, 0).substr(0, 5) == "ended", "process exit forces the final frame without waiting for a marker");

    TerminalModel burst(16, 2); TerminalDisplay batched;
    batched.update(burst, 0, 0, false, true);
    burst.feed("first"); batched.changed(0);
    check(!batched.update(burst, 0, 16) && line(batched, 0).empty(), "ordinary output waits briefly for adjacent chunks");
    check(!batched.update(burst, 0, 32, true), "queued bytes defer a partial burst");
    check(batched.update(burst, 0, 32) && line(batched, 0) == "first", "quiet output is presented promptly");
    for (uint64_t now = 100; now < 220; now += 10) {
        burst.feed("\rprogress"); batched.changed(now);
        check(!batched.update(burst, 0, now, true), "continuous output is coalesced within the delay bound");
    }
    check(batched.update(burst, 0, 220, true) && line(batched, 0) == "progress", "continuous output still presents progress within 120 ms");
    burst.resize(8, 3); batched.resized(300);
    check(!batched.update(burst, 0, 350) && batched.columns() == 16 && batched.rows() == 2, "resize paints retain a coherent previous frame");
    check(batched.update(burst, 0, 400) && batched.columns() == 8 && batched.rows() == 3, "resize settles even if the application emits no more output");
    for (uint64_t now = 500; now < 620; now += 10) {
        burst.resize(now % 20 ? 9 : 8, 3); batched.resized(now);
        check(!batched.update(burst, 0, now), "consecutive resizes are coalesced");
    }
    check(batched.update(burst, 0, 620), "continuous resizing cannot defer presentation forever");
    burst.feed("\r\na\r\nb\r\nc"); batched.changed(700);
    check(batched.update(burst, 1, 732) && !batched.cursorVisible() && line(batched, 0) == line(burst, 0, 1), "scrollback is captured with its cursor hidden");
}
void terminalScrollTests() {
    TerminalModel model(16, 3); TerminalDisplay display;
    for (int i = 0; i < 12; ++i) model.feed("line " + std::to_string(i) + "\r\n");
    display.update(model, 0, 0, false, true);
    uint64_t now = 1;
    for (size_t scroll : {1u, 2u, 3u, 4u, 3u, 2u, 1u, 0u}) {
        check(display.update(model, scroll, now++), "each scroll position is presented immediately during continuous wheel input");
        check(line(display, 0) == line(model, 0, scroll) && display.cursorVisible() == (scroll == 0), "scroll presents the requested history and cursor state");
        check(!display.pending(), "scrolling alone does not start an output batching timer");
    }

    model.feed("new"); display.changed(20);
    check(!display.update(model, 0, 21, true), "new output is still batched before scrolling");
    check(display.update(model, 1, 21, true) && line(display, 0) == line(model, 0, 1), "scrolling bypasses the output wait even with queued output");
    model.feed("!"); display.changed(22);
    check(!display.update(model, 1, 23, true), "subsequent output remains batched at the same scroll position");
    check(display.update(model, 0, 23) && line(display, 2) == line(model, 2), "returning to the live screen is immediate");

    const std::string beforeSync = line(display, 0);
    model.feed("\x1b[?2026h\x1b[H\x1b[2Jincomplete"); display.changed(30);
    check(!display.update(model, 2, 31) && model.synchronizedOutput() && line(display, 0) == beforeSync, "scrolling cannot release synchronized output or expose a partial frame");
    model.feed("\x1b[?2026l"); display.changed(32);
    check(display.update(model, 2, 32) && line(display, 0) == line(model, 0, 2), "a scroll requested during synchronized output appears when the frame completes");
}
void terminalCopyTests() {
    TerminalModel model(12, 3); TerminalDisplay display;
    model.feed("A日本😀Z\r\n  two  \r\nlast");
    display.update(model, 0, 0, false, true);
    check(display.selectedText(0, 1) == "A", "copy can select exactly one character");
    check(display.selectedText(8, 0) == "A日本😀Z", "backward selection copies in reading order");
    check(display.selectedText(2, 3) == "日" && display.selectedText(1, 2) == "日", "copy includes a whole CJK character from either half");
    check(display.selectedText(5, 6) == "😀" && display.selectedText(6, 7) == "😀", "copy retains a whole supplementary Unicode character");
    check(display.selectionRange(6, 7) == std::make_pair(5, 7), "copy and highlighting use the same wide-character range");
    check(display.selectedText(2, 2).empty() && display.selectionRange(2, 2) == std::make_pair(2, 2), "a single click does not select even half of a wide character");
    check(display.selectedText(-1, 3).empty() && display.selectedText(1000, 1001).empty(), "absent and out-of-view selections are empty");
    check(display.selectedText(0, 19) == "A日本😀Z\r\n  two", "multiline copy uses CRLF, keeps indentation and drops row padding");
    check(display.selectedText(0, 1000) == "A日本😀Z\r\n  two\r\nlast", "selection extending past the screen is bounded");
    model.feed("\x1b[?2026h\x1b[H\x1b[2Jincomplete"); display.changed(10); display.update(model, 0, 10);
    check(display.selectedText(0, 8) == "A日本😀Z", "copy uses the displayed frame during synchronized output");

    TerminalModel history(8, 2); TerminalDisplay scrolled;
    history.feed("first\r\nsecond\r\nthird"); scrolled.update(history, 1, 0, false, true);
    check(scrolled.selectedText(0, 16) == "first\r\nsecond", "copy uses the visible scrollback rather than the live screen");
}
}
int main() {
    try { textTests(); terminalTests(); terminalDisplayTests(); terminalScrollTests(); terminalCopyTests(); std::cout << "Text and terminal core tests passed.\n"; return EXIT_SUCCESS; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return EXIT_FAILURE; }
}
