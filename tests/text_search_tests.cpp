#include "text_search.h"
#include "text.h"
#include "edit_buffer.h"
#include "diff.h"
#include <iostream>
#include <stdexcept>

using namespace poppy;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void finish(TextSearch& search, std::string_view text, size_t budget = 7) {
    size_t steps = 0;
    while (!search.step(text, budget)) check(++steps <= text.size() + 1, "Search did not make progress");
}
void navigation() {
    TextSearch search; const std::string text = "one ONE one";
    search.query("one"); finish(search, text);
    check(search.count() == 3 && search.ordinal() == 1 && search.current()->begin == 0, "Initial case-insensitive match failed");
    search.start(search.current()->end); finish(search, text);
    check(search.ordinal() == 2 && search.current()->begin == 4, "Next match failed");
    search.start(8, true); finish(search, text);
    check(search.ordinal() == 2 && search.current()->begin == 4, "Previous match failed");
    search.start(0, true); finish(search, text);
    check(search.ordinal() == 3 && search.current()->begin == 8, "Previous did not wrap");
    search.start(text.size()); finish(search, text);
    check(search.ordinal() == 1 && search.current()->begin == 0, "Next did not wrap");
    search.query("ONE", true); finish(search, text);
    check(search.count() == 1 && search.current()->begin == 4, "Case-sensitive search failed");
    search.query("missing"); finish(search, text);
    check(search.ready() && search.count() == 0 && !search.current(), "Missing query retained a match");
    search.query(""); finish(search, text);
    check(search.count() == 0 && !search.current(), "Empty query matched every offset");
    search.query("one"); finish(search, "");
    check(search.ready() && search.count() == 0, "Empty document did not finish");
}
void unicodeAndEncoding() {
    const std::string text = u8"日本語🙂\r\n日本語🙂\n";
    TextSearch search; search.query(u8"語🙂"); finish(search, text, 1);
    check(search.count() == 2 && search.current()->begin == 6 && search.current()->end == 13,
        "UTF-8 byte offsets or cross-slice matching failed");
    search.start(search.current()->end); finish(search, text, 1);
    check(search.current()->begin == 21 && search.current()->end == 28, "Unicode next match failed");
    std::string decoded, encoding, error;
    check(decodeText(std::string("\xff\xfeN\0e\0e\0d\0l\0e\0", 14), decoded, encoding, error), "UTF-16 fixture did not decode");
    search.query("needle"); finish(search, decoded, 1);
    check(search.count() == 1 && search.current()->end == 6, "Decoded UTF-16 document could not be searched");
}
void boundedWorkAndEdits() {
    TextSearch search; search.query("aaaaab");
    std::string repeated(2 * 1024 * 1024, 'a'); repeated += 'b';
    check(!search.step(repeated, 64), "Search ignored its work budget");
    // Replacing a query cancels the partially completed scan without retaining it.
    search.query("ab"); finish(search, repeated, 65536);
    check(search.count() == 1 && search.current()->end == repeated.size(), "Prefix fallback or query replacement failed");
    search.query("a"); finish(search, repeated, 65536);
    check(search.count() == repeated.size() - 1, "Frequent matches were capped");
    search.query("aa"); finish(search, "aaaaa", 1);
    check(search.count() == 2, "Matches should not overlap");
    search.query("aba");
    while (!search.step("ababa", 1, [](TextMatch match) { return match.begin != 0; })) {}
    check(search.count() == 1 && search.current()->begin == 2, "Rejected hidden text consumed an overlapping visible match");
    EditBuffer buffer; buffer.document.text = "old needle"; indexDocument(buffer.document); buffer.initialize(buffer.document.text);
    search.query("needle"); finish(search, buffer.document.text);
    check(search.current()->begin == 4, "Initial editor match failed");
    check(buffer.replace(0, 4, "new unsaved "), "Editing fixture failed");
    search.start(0); finish(search, buffer.document.text);
    check(search.current()->begin == 12 && buffer.dirty(), "Search did not use the unsaved document revision");
    search.query("longer than text"); finish(search, "short", 1);
    check(!search.current(), "Long query matched a short document");
}
void diffText() {
    DiffDocument diff;
    diff.patch.text = "diff --git a/needle b/needle\n--- a/needle\n+++ b/needle\n@@ -1,3 +1,3 @@\n context needle\n-old needle\n---needle\n+new needle\n+++needle\n"
        "diff --git a/other b/other\n--- a/other\n+++ b/other\n@@ -1 +1 @@\n-last needle\n+final needle\n";
    indexDiff(diff, true);
    const auto accept = [&](TextMatch match) { return diff.inlineRowForText(match.begin, match.end).has_value(); };
    TextSearch search; search.query("needle");
    while (!search.step(diff.patch.text, 7, accept)) {}
    check(search.count() == 7 && search.current()->begin == diff.patch.text.find("context needle") + 8, "Diff search included headers or omitted context/replacement lines");
    search.query("++"); while (!search.step(diff.patch.text, 1, accept)) {}
    check(search.count() == 1 && search.current()->begin == diff.patch.text.find("+++needle") + 1, "Diff prefix rejection consumed the first visible match");
    search.query("--"); while (!search.step(diff.patch.text, 1, accept)) {}
    check(search.count() == 1 && search.current()->begin == diff.patch.text.find("---needle") + 1, "Diff prefix rejection lost a removal match");
    const auto row = diff.inlineRowForText(search.current()->begin, search.current()->end);
    check(row && diff.displayRow(*row, true).before.number == 3, "Diff match did not map to its visible source row");
    check(!diff.inlineRowForText(0, 4) && !diff.inlineRowForText(0, 0) && !diff.inlineRowForText(0, diff.patch.text.size() + 1), "Diff accepted a metadata or invalid text range");
}
}
int main() {
    try { navigation(); unicodeAndEncoding(); boundedWorkAndEdits(); diffText(); std::cout << "Text search tests passed.\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
