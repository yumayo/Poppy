#include "terminal_link.h"
#include "terminal_display.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>

using namespace poppy;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void link(std::u32string_view text, std::u32string_view visible, const std::string& expected) {
    const auto start = text.find(visible);
    check(start != text.npos, "Missing link test fixture");
    for (size_t i = 0; i < text.size(); ++i) {
        const auto result = terminalLinkAt(text, i);
        check(result == (i >= start && i < start + visible.size() ? expected : ""), "URL text or clickable cell boundaries differ");
    }
    check(terminalLinkAt(text, text.size()).empty(), "Out-of-range URL position accepted");
}
void parsing() {
    link(U"Open http://localhost:3000/ now", U"http://localhost:3000/", "http://localhost:3000/");
    link(U"[HTTPS://example.com/a?x=1&y=2#part]", U"HTTPS://example.com/a?x=1&y=2#part", "HTTPS://example.com/a?x=1&y=2#part");
    link(U"(https://example.com/a_(b)).", U"https://example.com/a_(b)", "https://example.com/a_(b)");
    link(U"\"http://127.0.0.1:65535/test\"", U"http://127.0.0.1:65535/test", "http://127.0.0.1:65535/test");
    link(U"https://[::1]:443/a", U"https://[::1]:443/a", "https://[::1]:443/a");
    link(U"http://[2001:db8:0:0:0:0:0:1]/", U"http://[2001:db8:0:0:0:0:0:1]/", "http://[2001:db8:0:0:0:0:0:1]/");
    link(U"http://[::ffff:192.0.2.1]/", U"http://[::ffff:192.0.2.1]/", "http://[::ffff:192.0.2.1]/");
    link(U"https://example.com/%E6%97%A5?q=a%20b", U"https://example.com/%E6%97%A5?q=a%20b", "https://example.com/%E6%97%A5?q=a%20b");
    link(U"日本語 https://example.com/path。後続", U"https://example.com/path", "https://example.com/path");
    for (auto suffix : {U"<script>", U"\"extra", U"'extra", U"\\extra", U"\nextra", U"\t extra", U"\x1b extra",
                        U"%", U"%1", U"%GGextra", U"%0Aextra", U"%00extra", U"%7fextra", U"%5Cextra", U"\u202eextra"}) {
        const std::u32string visible = U"https://example.com/safe";
        link(visible + suffix, visible, "https://example.com/safe");
    }
    const auto multiple = U"http://first.test/ https://second.test/";
    check(terminalLinkAt(multiple, 3) == "http://first.test/" && terminalLinkAt(multiple, 25) == "https://second.test/", "Multiple URLs share their destination");
    for (std::u32string_view text : {U"http://", U"https:///path", U"https://?query", U"https://user:pass@example.com/",
            U"https://good.test@evil.test/", U"https://host:abc/", U"https://host:", U"https://host:/", U"https://host:0/", U"https://host:65536/",
            U"https://bad..host/", U"https://-host/", U"https://host-/", U"https://bad_host/", U"https://%65xample.com/",
            U"http://999.1.2.3/", U"http://127.1/", U"http://01.2.3.4/", U"http://[]/", U"http://[::::]/", U"http://[1::2::3]/",
            U"http://[:1:2:3:4:5:6:7]/", U"http://[1:2:3:4:5:6:7:8:9]/", U"http://[1:2:3:4:5:6:7:8:]/", U"http://[::gg]/",
            U"file://example.com/", U"javascript:alert(1)", U"ftp://example.com/", U"xhttps://example.com/", U"javascript:https://example.com/"}) {
        for (size_t i = 0; i < text.size(); ++i) check(terminalLinkAt(text, i).empty(), "Malformed or unsupported URL accepted");
    }
    const auto maximum = std::u32string(U"https://example.com/") + std::u32string(4076, U'a');
    check(maximum.size() == 4096 && terminalLinkAt(maximum, 0).size() == 4096, "URL at the length limit rejected");
    check(terminalLinkAt(maximum + U'a', 0).empty(), "Oversized URL opened as a truncated destination");
    const auto afterLong = maximum + U"a https://later.test/";
    check(terminalLinkAt(afterLong, maximum.size() + 2) == "https://later.test/", "An oversized URL disabled a later valid link");
}
void displayLinks() {
    TerminalModel model(60, 3); TerminalDisplay display;
    model.feed(u8"日本😀 https://example.com/a\x1b[31mb\x1b[0m END");
    display.update(model, 0, 0, true);
    check(display.linkAt(7, 0) == "https://example.com/ab", "Wide characters or ANSI colors shifted the URL");
    check(display.linkAt(29, 0).empty() && display.linkAt(-1, 0).empty() && display.linkAt(0, -1).empty() &&
        display.linkAt(60, 0).empty() && display.linkAt(0, 3).empty(), "Non-URL or invalid display coordinates accepted");

    const std::string url = "https://example.com/long/path?query=123";
    TerminalModel wrapped(12, 6); TerminalDisplay view;
    wrapped.feed(url + " "); view.update(wrapped, 0, 0, true);
    for (size_t i = 0; i < url.size(); ++i) check(view.linkAt(i % 12, i / 12) == url, "Automatic wrap split a URL");
    check(view.linkAt(url.size() % 12, url.size() / 12).empty(), "Space after wrapped URL is clickable");

    TerminalModel lines(24, 3);
    lines.feed("https://example.com/\r\nother/path"); view.update(lines, 0, 0, true);
    check(view.linkAt(0, 0) == "https://example.com/" && view.linkAt(0, 1).empty(), "Hard line break joined independent output");

    // Wrap metadata must survive scrollback without joining separate lines.
    TerminalModel history(12, 5);
    history.feed(url + "\r\nend\r\nmore\r\nlast");
    view.update(history, history.historySize(), 0, true);
    check(history.historySize() > 0 && history.wrappedLength(0, history.historySize()) == 12, "History lost automatic wrap metadata");
    check(view.linkAt(2, 0) == url && view.linkAt(2, 2) == url, "URL spanning history and screen was truncated");
    view.update(history, history.historySize() - 1, 0, true);
    check(view.linkAt(0, 0).empty(), "Partially visible URL above the viewport was opened");
    view.update(history, history.historySize(), 0, true);
    history.feed("\x1b[?1049hhttps://alt/ "); view.changed(); view.update(history, 0, 1);
    check(view.linkAt(0, 0) == "https://alt/", "Alternate screen used the normal screen URL");
    history.feed("\x1b[?1049l"); view.changed(); view.update(history, history.historySize(), 2);
    check(view.linkAt(0, 0) == url, "Normal URL was not restored after alternate screen");

    TerminalModel clipped(12, 2);
    clipped.feed("before\r\n" + url); view.update(clipped, clipped.historySize(), 0, true);
    check(view.linkAt(0, 1).empty(), "URL continuing below the viewport was opened partially");

    TerminalModel sync(60, 2);
    sync.feed("https://old.test/ "); view.update(sync, 0, 0, true);
    sync.feed("\x1b[?2026h\r\x1b[Khttps://new.test/ "); view.changed();
    check(!view.update(sync, 0, 1) && view.linkAt(0, 0) == "https://old.test/", "Link hit testing used an undisplayed synchronized frame");
    sync.feed("\x1b[?2026l"); view.changed(); view.update(sync, 0, 2);
    check(view.linkAt(0, 0) == "https://new.test/", "Link did not follow the committed frame");

    history.resize(10, 5); view.update(history, history.historySize(), 0, true);
    check(!history.wrappedLength(0, history.historySize()), "Resize retained stale wrap links");
    history.feed("\x1b[3J\x1b" "c"); view.update(history, 0, 0, true);
    check(history.historySize() == 0 && view.linkAt(0, 0).empty(), "Reset retained a history URL");
}
}
int main() {
    try { parsing(); displayLinks(); std::cout << "Terminal URL tests passed.\n"; return EXIT_SUCCESS; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return EXIT_FAILURE; }
}
