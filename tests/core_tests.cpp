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
void terminalTitleTests() {
    TerminalModel terminal(12, 3), other;
    check(terminal.title().empty(), "terminal starts without a process title");
    terminal.feed("abc\x1b]0;PowerShell\a");
    check(terminal.title() == "PowerShell", "OSC 0 sets the title without a reply handler");
    terminal.feed("\x1b]2;Codex; project\x1b\\");
    check(terminal.title() == "Codex; project", "OSC 2 accepts ST and semicolons in titles");
    check(line(terminal, 0) == "abc" && terminal.cursorX() == 3 && terminal.cursorY() == 0,
          "title changes do not print text or move the cursor");
    check(other.title().empty(), "terminal titles are isolated between sessions");

    terminal.feed("\x1b]2;日本");
    check(terminal.title() == "Codex; project", "unfinished OSC does not change the title");
    terminal.feed("語 😀\x1b");
    check(terminal.title() == "Codex; project", "split ST waits for its final byte");
    terminal.feed("\\");
    check(terminal.title() == "日本語 😀", "split OSC preserves Unicode titles");
    const std::string bytewise = "\x1b]0;分割 😀\x1b\\";
    for (const char c : bytewise) terminal.feed(std::string_view(&c, 1));
    check(terminal.title() == "分割 😀", "OSC and UTF-8 can be split at every byte");

    std::string response;
    terminal.reply = [&](std::string bytes) { response += bytes; };
    terminal.feed("\x1b]1;icon only\a\x1b]8;;https://example.test\x1b\\\x1b]52;c;aGVsbG8=\a");
    check(terminal.title() == "分割 😀" && response.empty(), "unrelated OSC commands do not change the title or reply");
    terminal.feed("\x1b]11;?\a");
    check(response == "\x1b]11;rgb:1616/1919/1f1f\x1b\\" && terminal.title() == "分割 😀",
          "theme queries still work after title updates");

    terminal.feed("\x1b]2;\a");
    check(terminal.title().empty(), "an empty title clears the process title");
    terminal.feed("\x1b]0;\x1b\\");
    check(terminal.title().empty(), "OSC 0 can also clear the title");
    terminal.feed("\x1b]2;line\n\t\r\x7f\xc2\x85title\a");
    check(terminal.title() == "linetitle", "control characters are removed from titles");
    terminal.feed("\x1b]2;bad\xe3\x81\a");
    check(terminal.title() == "bad\xef\xbf\xbd\xef\xbf\xbd", "incomplete UTF-8 is replaced when OSC terminates");

    const std::string longest(254, 'x');
    terminal.feed("\x1b]2;" + longest + "\a");
    check(terminal.title() == longest, "OSC title at the sequence size limit is accepted");
    terminal.feed("\x1b]2;" + std::string(255, 'y') + "\aZ");
    check(terminal.title() == longest && line(terminal, 0) == "abcZ", "oversized OSC is discarded through BEL");
    terminal.feed("\x1b]0;" + std::string(4096, 'z') + "0;forged\x1b");
    terminal.feed("\\Q");
    check(terminal.title() == longest && line(terminal, 0) == "abcZQ", "oversized split OSC is discarded through ST");
    terminal.feed("\x1b]2;recovered\a");
    check(terminal.title() == "recovered", "title parser recovers after oversized OSC");
    terminal.feed("\x1b" "c");
    check(terminal.title().empty(), "terminal reset clears the previous process title");
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
    model.feed("old"); display.update(model, 0, 0, true);
    model.feed("\x1b[?2026h\x1b[H\x1b[2J\x1b[31m途中\x1b[?25l"); display.changed();
    check(!display.update(model, 0, 10) && display.pending(), "begin marker defers presentation");
    check(!display.update(model, 0, 500) && line(display, 0) == "old" && display.cursorX() == 3 && display.cursorVisible(), "incidental paints retain committed text and cursor");
    model.feed("\x1b[H\x1b[2J完成\x1b[?25h\x1b[?2026"); display.changed();
    check(!display.update(model, 0, 501) && line(display, 0) == "old", "split end marker does not expose intermediate text");
    model.feed("l"); display.changed();
    check(display.update(model, 0, 502) && line(display, 0) == "完成" && display.cursorX() == 4 && !display.pending(), "completed frame is presented with its cursor");
    check(display.row(0)[0].fg == model.row(0)[0].fg && display.row(0)[1].continuation, "committed frame retains colors and wide cells");

    // Simulate resume output split into the same 8 KiB chunks used by the UI.
    std::string replay = "\x1b[?2026h";
    for (int i = 0; i < 4000; ++i) replay += "history entry\r\n";
    replay += "\x1b[H\x1b[2Jready\x1b[?2026l";
    uint64_t tick = 600;
    for (size_t offset = 0; offset < replay.size(); offset += 8192) {
        model.feed(std::string_view(replay).substr(offset, 8192)); display.changed();
        const bool last = offset + 8192 >= replay.size();
        check(display.update(model, 0, tick) == last, "replayed history is only presented after its end marker");
        check(line(display, 0) == (last ? "ready" : "完成"), "resume does not flash replayed history");
        ++tick;
    }

    model.feed("\x1b[?2026h\rtimeout"); display.changed();
    check(!display.update(model, 0, 1000), "incomplete synchronized output waits");
    model.feed("\x1b[?2026h"); display.changed();
    check(!display.update(model, 0, 1999), "repeated begin marker does not restart the timeout");
    check(display.update(model, 0, 2000) && !model.synchronizedOutput() && !display.pending(), "missing end marker is released after one second without more output");
    model.feed("\x1b[?2026h\rended"); display.changed(); display.update(model, 0, 2010);
    check(display.update(model, 0, 2011, true) && !model.synchronizedOutput() && line(display, 0).substr(0, 5) == "ended", "process exit forces the final frame without waiting for a marker");
}
void terminalInputTests() {
    TerminalModel model(80, 3); TerminalDisplay display;
    model.feed(std::string(60, 'x'));
    display.update(model, 0, 0, true);
    for (uint64_t now = 1; now <= 10; ++now) {
        check(!display.pending() && !display.update(model, 0, now), "typing alone does not redraw an unchanged frame");
        model.feed("\b"); display.changed();
        check(display.update(model, 0, now) && display.cursorX() == model.cursorX(), "backspace moves the displayed cursor immediately");
        model.feed("\x1b[K"); display.changed();
        check(display.update(model, 0, now), "a split erase is presented in the same clock tick without a frame delay");
        check(line(display, 0) == std::string(static_cast<size_t>(model.cursorX()), 'x'), "backspace immediately presents the erased text");
    }

    model.feed("!"); display.changed();
    check(display.update(model, 0, 500), "delayed echo is presented as soon as it arrives");
    model.feed("?"); display.changed();
    check(display.update(model, 0, 500), "ordinary output is also immediate in the same clock tick");
    for (uint64_t now = 501; now <= 550; ++now) {
        model.feed("\r\x1b[Kprogress " + std::to_string(now)); display.changed();
        check(display.update(model, 0, now) && line(display, 0) == "progress " + std::to_string(now), "continuous output never waits for quiet or a frame deadline");
    }
    for (uint64_t now = 551; now <= 570; ++now) {
        const int columns = now % 2 ? 40 : 80;
        model.resize(columns, 3); display.changed();
        check(display.update(model, 0, now) && display.columns() == columns && display.rows() == 3, "each resize is presented immediately without a settling timer");
    }
    check(!display.pending() && !display.update(model, 0, 600), "an idle terminal does not keep repainting");

    const std::string beforeSync = line(display, 0);
    model.feed("\x1b[?2026h\r\x1b[Kincomplete"); display.changed();
    check(!display.update(model, 0, 1000) && line(display, 0) == beforeSync && model.synchronizedOutput(), "immediate output still honors an application's synchronized frame");
    check(display.synchronizationDeadline() == 2000, "only explicit synchronized output schedules a timeout");
    model.feed("\r\x1b[Kcomplete\x1b[?2026l"); display.changed();
    check(display.update(model, 0, 1000) && line(display, 0) == "complete" && !display.synchronizationDeadline(), "a synchronized frame commits in the same clock tick as its end marker");

    for (const auto* prefix : {"$ ", "PS C:> ", "user@host:~$ "}) {
        TerminalModel shell(40, 3); TerminalDisplay prompt;
        shell.feed(prefix); prompt.update(shell, 0, 0, true);
        for (uint64_t now = 1; now <= 10; ++now) {
            shell.feed("\r\n"); prompt.changed();
            check(prompt.update(shell, 0, now) && prompt.cursorY() == shell.cursorY(), "each Enter immediately advances the displayed shell cursor");
            // Shells can send the prompt separately after evaluating its contents.
            shell.feed("\x1b[32m" + std::string(prefix) + "\x1b[0m"); prompt.changed();
            check(prompt.update(shell, 0, now) && line(prompt, prompt.cursorY()) == line(shell, shell.cursorY()) && prompt.cursorX() == shell.cursorX(),
                "every shell prompt is presented in the same clock tick as Enter without another debounce");
        }
    }
}
void terminalScrollTests() {
    TerminalModel model(16, 3); TerminalDisplay display;
    for (int i = 0; i < 12; ++i) model.feed("line " + std::to_string(i) + "\r\n");
    display.update(model, 0, 0, true);
    uint64_t now = 1;
    for (size_t scroll : {1u, 2u, 3u, 4u, 3u, 2u, 1u, 0u}) {
        check(display.update(model, scroll, now++), "each scroll position is presented immediately during continuous wheel input");
        check(line(display, 0) == line(model, 0, scroll) && display.cursorVisible() == (scroll == 0), "scroll presents the requested history and cursor state");
        check(!display.pending(), "scrolling alone does not start an output batching timer");
    }

    model.feed("new"); display.changed();
    check(display.update(model, 0, 20), "new output is immediate before scrolling");
    check(display.update(model, 1, 21) && line(display, 0) == line(model, 0, 1), "scrolling immediately presents the requested history");
    model.feed("!"); display.changed();
    check(display.update(model, 1, 22), "new output is immediate at the same scroll position");
    check(display.update(model, 0, 23) && line(display, 2) == line(model, 2), "returning to the live screen is immediate");

    const std::string beforeSync = line(display, 0);
    model.feed("\x1b[?2026h\x1b[H\x1b[2Jincomplete"); display.changed();
    check(!display.update(model, 2, 31) && model.synchronizedOutput() && line(display, 0) == beforeSync, "scrolling cannot release synchronized output or expose a partial frame");
    model.feed("\x1b[?2026l"); display.changed();
    check(display.update(model, 2, 32) && line(display, 0) == line(model, 0, 2), "a scroll requested during synchronized output appears when the frame completes");
}
void terminalCopyTests() {
    TerminalModel model(12, 3); TerminalDisplay display;
    model.feed("A日本😀Z\r\n  two  \r\nlast");
    display.update(model, 0, 0, true);
    check(display.selectedText(0, 1) == "A", "copy can select exactly one character");
    check(display.selectedText(8, 0) == "A日本😀Z", "backward selection copies in reading order");
    check(display.selectedText(2, 3) == "日" && display.selectedText(1, 2) == "日", "copy includes a whole CJK character from either half");
    check(display.selectedText(5, 6) == "😀" && display.selectedText(6, 7) == "😀", "copy retains a whole supplementary Unicode character");
    check(display.selectionRange(6, 7) == std::make_pair(5, 7), "copy and highlighting use the same wide-character range");
    check(display.selectedText(2, 2).empty() && display.selectionRange(2, 2) == std::make_pair(2, 2), "a single click does not select even half of a wide character");
    check(display.selectedText(-1, 3).empty() && display.selectedText(1000, 1001).empty(), "absent and out-of-view selections are empty");
    check(display.selectedText(0, 19) == "A日本😀Z\r\n  two", "multiline copy uses CRLF, keeps indentation and drops row padding");
    check(display.selectedText(0, 1000) == "A日本😀Z\r\n  two\r\nlast", "selection extending past the screen is bounded");
    model.feed("\x1b[?2026h\x1b[H\x1b[2Jincomplete"); display.changed(); display.update(model, 0, 10);
    check(display.selectedText(0, 8) == "A日本😀Z", "copy uses the displayed frame during synchronized output");

    TerminalModel history(8, 2); TerminalDisplay scrolled;
    history.feed("first\r\nsecond\r\nthird"); scrolled.update(history, 1, 0, true);
    check(scrolled.selectedText(0, 16) == "first\r\nsecond", "copy uses the visible scrollback rather than the live screen");
}
void terminalSelectionScrollTests() {
    TerminalModel model(8, 2); TerminalDisplay display;
    model.feed("first\r\nsecond\r\nthird\r\nfourth\r\nfifth");
    display.update(model, 0, 0, true);
    display.beginSelection(0); display.extendSelection(model, 6);
    check(display.hasSelection() && display.selectedText() == "fourth", "drag selects the committed terminal text");
    display.update(model, 1, 1);
    check(display.selectionRange() == std::make_pair(8, 14) && display.selectedText() == "fourth", "scrolling moves highlighting with the selected row");
    display.update(model, 3, 2);
    check(display.hasSelection() && display.selectionRange() == std::make_pair(16, 16) && display.selectedText() == "fourth", "selection and copy survive scrolling completely off screen");
    display.update(model, 0, 3);
    check(display.selectionRange() == std::make_pair(0, 6), "scrolling back restores the original highlight");
    display.beginSelection(6); display.extendSelection(model, 0);
    display.update(model, 3, 4);
    display.extendSelection(model, 0);
    check(display.selectedText() == "first\r\nsecond\r\nthird\r\nfourth", "backward drag across scroll positions includes hidden intervening rows");
    check(display.selectionRange() == std::make_pair(0, 16), "a selection spanning the viewport highlights all visible cells");
    display.beginSelection(2); display.update(model, 0, 5); display.extendSelection(model, 13);
    check(display.selectedText() == "rst\r\nsecond\r\nthird\r\nfourth\r\nfifth", "forward drag across scroll positions keeps both endpoints and row boundaries");
    display.clearSelection();
    check(!display.hasSelection() && display.selectedText().empty() && display.selectionRange() == std::make_pair(0, 0), "clearing also discards an off-screen selection");
    display.beginSelection(0); display.extendSelection(model, 0);
    check(!display.hasSelection() && display.selectedText().empty(), "a click still does not select text");

    TerminalModel unicode(12, 2); TerminalDisplay wide;
    unicode.feed("A日本😀Z\r\nnext"); wide.update(unicode, 0, 0, true);
    wide.beginSelection(2); wide.extendSelection(unicode, 6);
    unicode.feed("\r\nlast"); wide.changed(); wide.update(unicode, 0, 40);
    check(wide.hasSelection() && wide.selectionRange() == std::make_pair(0, 0) && wide.selectedText() == "日本😀", "output scroll keeps the selected Unicode text off screen");
    wide.update(unicode, 1, 41);
    check(wide.selectionRange() == std::make_pair(1, 7) && wide.selectedText() == "日本😀", "wide-character highlight is restored on returning to history");
    unicode.feed("\x1b[?2026h\x1b[H\x1b[2Jpartial"); wide.changed();
    check(!wide.update(unicode, 0, 42), "selection scrolling respects a synchronized frame");
    wide.beginSelection(2); wide.extendSelection(unicode, 6);
    check(wide.selectedText() == "日本😀", "selecting during synchronized output reads the displayed cells");
    unicode.feed("\x1b[?2026l"); wide.changed(); wide.update(unicode, 0, 43);
    check(wide.hasSelection() && wide.selectedText() == "日本😀", "a delayed scroll commit preserves selected text");

    TerminalModel bounded(8, 2); TerminalDisplay history;
    for (int i = 0; i < 1002; ++i) bounded.feed("row\r\n");
    history.update(bounded, 0, 0, true);
    history.beginSelection(0); history.extendSelection(bounded, 3);
    bounded.feed("next\r\n"); history.changed(); history.update(bounded, 1, 40);
    check(history.selectionRange() == std::make_pair(0, 3) && history.selectedText() == "row", "history eviction does not shift the selection onto a different row");
    history.update(bounded, bounded.historySize(), 41);
    history.beginSelection(0); history.extendSelection(bounded, 3);
    bounded.feed("drop\r\n"); history.changed(); history.update(bounded, bounded.historySize(), 80);
    check(!history.hasSelection(), "selection is cleared when its text leaves the history budget");
    history.beginSelection(0); history.extendSelection(bounded, 3);
    bounded.feed("\x1b[3J"); history.changed(); history.update(bounded, 0, 120);
    check(!history.hasSelection(), "erasing selected history clears its selection");
    history.beginSelection(0); history.extendSelection(bounded, 3);
    bounded.feed("\x1b[?1049hALT\x1b[?1049l"); history.changed(); history.update(bounded, 0, 160);
    check(!history.hasSelection(), "switching terminal buffers invalidates old selection coordinates");
    history.beginSelection(0); history.extendSelection(bounded, 3);
    bounded.resize(10, 3); history.changed(); history.update(bounded, 0, 300);
    check(!history.hasSelection(), "resizing invalidates the selected cell coordinates");
    history.beginSelection(0); history.extendSelection(bounded, 3);
    bounded.feed("\x1b" "c"); history.changed(); history.update(bounded, 0, 340);
    check(!history.hasSelection(), "terminal reset clears selection without reusing old row coordinates");
}
void terminalPreeditTests() {
    const auto previewLine = [](const TerminalPreedit& preview, int y) {
        auto cells = preview.cells[y];
        for (const auto& glyph : preview.composition.glyphs) if (glyph.row == y) {
            cells[glyph.column].ch = glyph.character;
            cells[glyph.column].continuation = false;
            if (glyph.width == 2) { cells[glyph.column + 1].ch = 0; cells[glyph.column + 1].continuation = true; }
        }
        std::string text;
        for (const auto& cell : cells) if (!cell.continuation) appendUtf8(text, cell.ch);
        while (!text.empty() && text.back() == ' ') text.pop_back();
        return text;
    };
    TerminalModel model(24, 4); TerminalDisplay display;
    model.feed("abc\x1b[38;2;1;2;3mdef\x1b[0m\r\x1b[4G");
    display.update(model, 0, 0, true);
    auto preview = display.preedit({u"にほんご", {}, 4});
    check(previewLine(preview, 0) == "abcにほんごdef", "preedit inserts before existing text");
    check(preview.cells[0][11].ch == U'd' && preview.cells[0][11].fg == rgb(1,2,3), "shifted suffix retains its colors");
    check(line(model, 0) == "abcdef" && line(display, 0) == "abcdef" && model.cursorX() == 3,
        "preedit never changes the terminal buffer or its real cursor");
    preview = display.preedit({u"日本語", {}, 0});
    check(previewLine(preview, 0) == "abc日本語def" && preview.composition.caretColumn == 3,
        "conversion shrinks the gap using the full preedit rather than its cursor");
    preview = display.preedit({u"日本語😀ｱ", {}, 4});
    check(previewLine(preview, 0) == "abc日本語😀ｱdef" && preview.composition.caretColumn == 9,
        "emoji and halfwidth text use cell widths and UTF-16 cursor positions");
    check(display.preedit({}).cells.empty() && line(display, 0) == "abcdef", "cancel immediately restores the original frame");
    model.feed("\x1b[6@日本語"); display.changed(); display.update(model, 0, 1);
    check(line(display, 0) == "abc日本語def" && display.preedit({}).cells.empty(), "committed output takes over without duplicate text");

    TerminalModel wrapped(8, 5); TerminalDisplay multiline;
    wrapped.feed("abcdefghij\x1b[1;4H"); multiline.update(wrapped, 0, 0, true);
    preview = multiline.preedit({u"日本語", {}, 3});
    check(previewLine(preview, 0) == "abc日本" && previewLine(preview, 1) == "語defghi" && previewLine(preview, 2) == "j",
        "preedit reflows an automatically wrapped suffix without splitting wide glyphs");
    check(preview.composition.caretRow == 1 && preview.composition.caretColumn == 2, "wrapped candidates follow the preedit caret");
    check(line(multiline, 0) == "abcdefgh" && line(multiline, 1) == "ij", "wrapped preview is reversible");
    wrapped.feed("\x1b[2;3Hkl\x1b[1;4H"); multiline.changed(); multiline.update(wrapped, 0, 1);
    preview = multiline.preedit({u"日", {}, 1});
    check(previewLine(preview, 1) == "ghijkl", "fresh terminal output is used when rebuilding the preview");

    TerminalModel padding(5, 4); TerminalDisplay padded;
    padding.feed("abcd日本Z\x1b[1;3H"); padded.update(padding, 0, 0, true);
    preview = padded.preedit({u"X", {}, 1});
    check(previewLine(preview, 0) == "abXcd" && previewLine(preview, 1) == "日本Z", "wide-wrap padding is not inserted into the suffix");

    TerminalModel edge(4, 3); TerminalDisplay delayed;
    edge.feed("abcd"); delayed.update(edge, 0, 0, true);
    preview = delayed.preedit({u"日", {}, 1});
    check(previewLine(preview, 0) == "abcd" && previewLine(preview, 1) == "日", "pending autowrap inserts after the final column");
    edge.feed("\x1b[1;4H"); delayed.changed(); delayed.update(edge, 0, 1);
    preview = delayed.preedit({u"日", {}, 1});
    check(previewLine(preview, 0) == "abc" && previewLine(preview, 1) == "日d", "wide preedit at the edge leaves padding before wrapping");

    TerminalModel hard(8, 3); TerminalDisplay bounded;
    hard.feed("abcde\r\nSTATUS\x1b[1;4H"); bounded.update(hard, 0, 0, true);
    preview = bounded.preedit({u"日本語", {}, 3});
    check(previewLine(preview, 1) == "STATUS" && preview.lastRow == 0 && preview.composition.caretRow == 0,
        "long preedit pans within its input row without overwriting independent output");
    check(previewLine(preview, 0) == "語de", "overflow shows the active preedit row and shifted suffix");
    preview = bounded.preedit({u"日", {}, 1});
    check(previewLine(preview, 0) == "abc日de" && previewLine(preview, 1) == "STATUS", "shortening a conversion restores the hidden prefix");

    TerminalModel box(12, 3); TerminalDisplay framed;
    box.feed("│ abcdef   │\r\n└──────────┘\x1b[1;6H"); framed.update(box, 0, 0, true);
    preview = framed.preedit({u"日", {}, 1});
    check(previewLine(preview, 0) == "│ abc日def │" && previewLine(preview, 1) == "└──────────┘",
        "CLI borders stay fixed while the text inside moves");
    check(preview.left == 1 && preview.right == 11 && preview.lastRow == 0, "framed preedit is clipped to the input interior");
    preview = framed.preedit({u"日本語日本語", {}, 6});
    check(preview.cells[0][0].ch == U'│' && preview.cells[0][11].ch == U'│' && preview.composition.caretRow == 0,
        "overflow never moves or overwrites a frame");

    TerminalModel wide(12, 2); TerminalDisplay inside;
    wide.feed("A日本Z\x1b[1;3H"); inside.update(wide, 0, 0, true);
    preview = inside.preedit({u"X", {}, 1});
    check(previewLine(preview, 0) == "AX日本Z", "a cursor inside a wide cell does not split the existing glyph");

    TerminalModel panel(12, 2); TerminalDisplay colored;
    panel.feed("\x1b[48;2;30;40;50m\x1b[2Kabcdef\x1b[1;4H"); colored.update(panel, 0, 0, true);
    preview = colored.preedit({u"日", {}, 1});
    check(previewLine(preview, 0) == "abc日def" && preview.compositionBackground == rgb(30,40,50) &&
        preview.cells[0][8].bg == rgb(30,40,50) && preview.cells[1][0].bg == background,
        "preedit preserves the input panel background without changing adjacent blank rows");

    hard.feed("\x1b[3;1Hlast\r\nnext"); bounded.changed(); bounded.update(hard, 1, 1);
    check(bounded.preedit({u"日", {}, 1}).cells.empty(), "preedit never modifies a scrollback view");
    bounded.update(hard, 0, 2);
    preview = bounded.preedit({u"日本語", {}, 3});
    check(preview.composition.caretRow == 2 && preview.composition.caretColumn < hard.columns(), "bottom-row candidates stay in the viewport");
}
void terminalWrapMetadataTests() {
    TerminalModel model(5, 3);
    model.feed("abcd日Z");
    check(model.wrappedLength(0) == 4 && model.wrappedLength(1) == 0, "wide autowrap records the content length without padding");
    model.feed("\x1b[1;5H\n");
    check(!model.wrappedLength(0), "explicit line feed breaks a previous automatic continuation");
    model.feed("\x1b[HabcdefghijklmnoP");
    check(model.wrappedLength(0) == 5 && model.wrappedLength(1) == 5 && !model.wrappedLength(2), "scrolling preserves live automatic continuations");
    model.feed("\x1b[?1049h");
    check(!model.wrappedLength(0), "alternate screens start without old wrap metadata");
    model.feed("\x1b[?1049l");
    check(model.wrappedLength(0) == 5, "returning to the normal screen restores its wrap metadata");
    model.feed("\x1b[1;1H\x1b[K");
    check(!model.wrappedLength(0), "erasing to the right edge clears a continuation");
    model.resize(6, 2);
    check(!model.wrappedLength(0) && !model.wrappedLength(1), "resize discards stale wrap links");
    model.feed("\x1b" "c");
    check(!model.wrappedLength(0) && !model.wrapPending(), "reset clears wrap metadata");
}
}
int main() {
    try { textTests(); terminalTests(); terminalTitleTests(); terminalDisplayTests(); terminalInputTests(); terminalScrollTests(); terminalCopyTests(); terminalSelectionScrollTests(); terminalPreeditTests(); terminalWrapMetadataTests(); std::cout << "Text and terminal core tests passed.\n"; return EXIT_SUCCESS; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return EXIT_FAILURE; }
}
