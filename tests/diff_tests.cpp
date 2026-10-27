#include "diff.h"
#include "diff_layout.h"
#include "git_history.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <utility>

using namespace poppy;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
DiffDocument parse(std::string patch) {
    DiffDocument result; result.patch.text = std::move(patch); indexDiff(result); return result;
}
Style styleAt(const DiffDocument& diff, DiffLine source, size_t byte) {
    auto state = source.state;
    std::vector<Token> tokens;
    lexLine(diff.line(source), source.language, state, &tokens);
    for (const auto& token : tokens) if (token.begin <= byte && byte < token.end) return token.style;
    return Style::Text;
}
void syntaxHighlighting() {
    const auto diff = parse(
        "diff --git a/file.CPP b/file.CPP\n--- a/file.CPP\n+++ b/file.CPP\n@@ -1,3 +1,4 @@\n"
        " int unchanged = 1;\n-const char* value = \"before\"; // 古い\n"
        "+const char* value = \"after\"; // 新しい\n+return 2;\n // tail\n");
    for (const bool inlineView : {false, true}) for (size_t i = 0; i < diff.rowCount(inlineView); ++i) {
        const auto row = diff.displayRow(i, inlineView);
        for (const auto source : {row.before, row.after}) {
            if (!source.number) continue;
            const auto text = diff.line(source);
            check(source.language == Language::CLike, "Every diff layout and side must retain its file's language");
            if (text.substr(0, 2) == "//") check(styleAt(diff, source, 0) == Style::Comment, "Context comments lost their syntax style");
            else check(styleAt(diff, source, 0) == Style::Keyword, "Diff prefixes must not shift keyword token offsets");
            const auto quote = text.find('"'), comment = text.find("//"), digit = text.find_first_of("12");
            if (quote != text.npos) check(styleAt(diff, source, quote + 1) == Style::String, "Changed strings lost their syntax style");
            if (comment != text.npos) check(styleAt(diff, source, comment + 3) == Style::Comment, "UTF-8 comments lost their syntax style");
            if (digit != text.npos) check(styleAt(diff, source, digit) == Style::Number, "Diff numbers lost their syntax style");
        }
    }
    check(diff.selectedText(true, 0, diff.patch.text.size()) == "int unchanged = 1;\nconst char* value = \"after\"; // 新しい\nreturn 2;\n// tail\n",
        "Syntax indexing must not alter copied source text");
}
void multilineSyntax() {
    const auto diff = parse("--- a/file.cpp\n+++ b/file.cpp\n@@ -1,6 +1,6 @@\n"
        " /* shared\n context\n-*/ int old = 1;\n+new comment\n int between = 2;\n-tail\n+*/ int fresh = 3;\n return 4;\n");
    check(styleAt(diff, diff.rows[1].before, 0) == Style::Comment && styleAt(diff, diff.rows[1].after, 0) == Style::Comment,
        "Multiline comments must continue on both sides");
    check(styleAt(diff, diff.rows[2].before, 3) == Style::Keyword && styleAt(diff, diff.rows[2].after, 0) == Style::Comment,
        "A removed comment terminator must affect only the original side");
    check(styleAt(diff, diff.rows[3].before, 0) == Style::Keyword && styleAt(diff, diff.rows[3].after, 0) == Style::Comment,
        "Shared context must retain independent before/after lexer states");
    check(styleAt(diff, diff.rows[4].after, 3) == Style::Keyword && styleAt(diff, diff.rows[5].before, 0) == Style::Keyword &&
        styleAt(diff, diff.rows[5].after, 0) == Style::Keyword, "Added comment terminators must restore subsequent keywords");
    for (const auto& fixture : std::vector<std::pair<std::string, std::string>>{{"py", "\"\"\""}, {"ts", "`"}}) {
        const auto strings = parse("--- a/file." + fixture.first + "\n+++ b/file." + fixture.first + "\n@@ -1,3 +1,3 @@\n-" +
            fixture.second + "old\n+" + fixture.second + "new\n body\n " + fixture.second + "\n");
        check(styleAt(strings, strings.rows[1].before, 0) == Style::String && styleAt(strings, strings.rows[1].after, 0) == Style::String,
            "Python triple strings and C-like template literals must span diff lines");
    }
    const auto hunks = parse("--- a/file.cpp\n+++ b/file.cpp\n@@ -1 +1 @@\n-/* old\n+/* new\n"
        "@@ -20 +20 @@\n-return 1;\n+return 2;\n");
    check(styleAt(hunks, hunks.rows[1].before, 0) == Style::Keyword && styleAt(hunks, hunks.rows[1].after, 0) == Style::Keyword,
        "Lexer state must not leak across omitted source ranges");
}
void syntaxLanguages() {
    const std::vector<std::pair<std::string, Language>> files{
        {"src/日本語.CPP", Language::CLike}, {"src/with spaces.py\t", Language::Python},
        {"script.sh", Language::Shell}, {"data.json", Language::Json}, {"config.yaml", Language::Config},
        {"dir/CMakeLists.txt", Language::Config}, {"unknown.xyz", Language::Plain}, {"no-extension", Language::Plain}, {".py", Language::Plain},
        {"\"dir/quoted\\\"name.py\"", Language::Python}, {"\"dir/escaped\\tname.cpp\"", Language::CLike},
        {"\"dir/\\346\\227\\245\\346\\234\\254.py\"", Language::Python}};
    for (const auto& file : files) {
        const auto diff = parse("--- " + file.first + "\n+++ " + file.first + "\n@@ -1 +1 @@\n-return \"日本語\" + 41\n+return \"日本語\" + 42\n");
        const auto row = diff.rows[0];
        for (const auto source : {row.before, row.after}) {
            check(source.language == file.second, "Diff language detection lost a supported suffix or a quoted filename");
            const auto text = diff.line(source);
            const bool plain = file.second == Language::Plain;
            check(styleAt(diff, source, 0) == (plain ? Style::Text : Style::Keyword) &&
                styleAt(diff, source, 8) == (plain ? Style::Text : Style::String) &&
                styleAt(diff, source, text.size() - 1) == (plain ? Style::Text : Style::Number),
                "Diff languages must use the same token styles as normal documents");
        }
    }
    const auto added = parse(untrackedPatch("new.py", "return 42\n"));
    check(added.rows[0].after.language == Language::Python && styleAt(added, added.rows[0].after, 0) == Style::Keyword,
        "Untracked files in All Changes must use their source language");
    const auto deleted = parse("--- a/deleted.py\n+++ /dev/null\n@@ -1 +0,0 @@\n-return 42\n");
    check(deleted.rows[0].before.language == Language::Python && styleAt(deleted, deleted.rows[0].before, 0) == Style::Keyword,
        "Deleted files must use the original path rather than /dev/null");
    auto combined = parse("diff --git a/one.cpp b/one.cpp\n--- a/one.cpp\n+++ b/one.cpp\n@@ -1 +1 @@\n-/* old\n+/* new\n" +
        untrackedPatch("two.py", "return 42\n") + "diff --git a/plain b/plain\n@@ -1 +1 @@\n-before\n+after\n");
    for (const bool headings : {false, true}) {
        indexDiff(combined, headings);
        for (const auto& row : combined.rows) {
            if (combined.line(row.after) == "return 42") check(row.after.language == Language::Python && styleAt(combined, row.after, 0) == Style::Keyword,
                "All Changes must reset the language and multiline state for each file");
            if (combined.line(row.after) == "after") check(row.after.language == Language::Plain && row.after.state == LexState::Normal,
                "A file without source headers must not inherit the preceding file's language");
        }
    }
    combined.patch.text = "@@ -1 +1 @@\n-return 1\n+return 2\n"; indexDiff(combined);
    check(styleAt(combined, combined.rows[0].after, 0) == Style::Text, "Reindexing retained a previous file's syntax metadata");
}
void alignment() {
    const auto diff = parse(
        "diff --git a/file.cpp b/file.cpp\nindex 1234567..7654321 100644\n"
        "--- a/file.cpp\n+++ b/file.cpp\n@@ -10,5 +20,6 @@ function\n"
        " context\n-old one\n-old two\n+new one\n unchanged\n+extra one\n+extra two\n last\n"
        "@@ -40 +70 @@\n-before\n+after\n");
    check(diff.rows.size() == 8, "Metadata must not produce display rows");
    const auto& context = diff.rows[0];
    check(!context.changed && context.before.number == 10 && context.after.number == 20 &&
        diff.line(context.before) == "context" && diff.line(context.after) == "context", "Context must use each file's line numbers");
    const auto& replacement = diff.rows[1];
    check(replacement.changed && replacement.before.number == 11 && replacement.after.number == 21 &&
        diff.line(replacement.before) == "old one" && diff.line(replacement.after) == "new one", "Replacement lines must align");
    check(diff.rows[2].changed && diff.rows[2].before.number == 12 && !diff.rows[2].after.number,
        "Unpaired removals need an empty cell on the right");
    check(diff.rows[3].before.number == 13 && diff.rows[3].after.number == 22 && !diff.rows[3].changed,
        "Context after uneven replacements must stay aligned");
    check(!diff.rows[4].before.number && diff.rows[4].after.number == 23 && diff.rows[5].after.number == 24,
        "Unpaired additions need empty cells on the left");
    check(diff.rows[6].before.number == 14 && diff.rows[6].after.number == 25,
        "Insertions must advance only the new file's line numbers");
    check(diff.rows[7].before.number == 40 && diff.rows[7].after.number == 70,
        "Each hunk must restore source line numbers, including omitted counts");
    check(diff.maxColumns == 9, "Scrollbar width must exclude prefixes and metadata");
    check(diff.selectedText(false, 0, diff.patch.text.size()) == "context\nold one\nold two\nunchanged\nlast\nbefore\n",
        "Copying the original side must omit metadata, prefixes and additions");
    check(diff.selectedText(true, 0, diff.patch.text.size()) == "context\nnew one\nunchanged\nextra one\nextra two\nlast\nafter\n",
        "Copying the new side must omit metadata, prefixes and removals");
    const size_t start = diff.offset(replacement.after);
    check(diff.selectedText(true, start + 1, start + 5) == "ew o" && diff.selectedText(true, start + 5, start + 1) == "ew o",
        "Partial and backward selections must copy the selected bytes");
}
void additionsAndDeletions() {
    const auto added = parse("new file mode 100644\n--- /dev/null\n+++ b/new\n@@ -0,0 +1,3 @@\n+\n+--- literal\n+日本語\n");
    check(added.rows.size() == 3 && !added.rows[0].before.number && added.rows[0].after.number == 1 && added.line(added.rows[0].after).empty(),
        "A new file may begin with an empty line");
    check(added.line(added.rows[1].after) == "--- literal" && added.maxColumns == 11,
        "Source text resembling metadata must remain visible");
    check(added.selectedText(true, 0, added.patch.text.size()) == "\n--- literal\n日本語\n", "Copy must preserve blank lines and UTF-8");
    const auto removed = parse("deleted file mode 100644\n@@ -1,2 +0,0 @@\n-+++ literal\n-@@ source\n\\ No newline at end of file\n");
    check(removed.rows.size() == 2 && !removed.rows[1].after.number && removed.rows[1].before.number == 2,
        "Deleted files must leave the right side empty");
    check(removed.selectedText(false, 0, removed.patch.text.size()) == "+++ literal\n@@ source",
        "Copy must preserve a missing final newline and source text resembling metadata");
    check(removed.selectedText(true, 0, removed.patch.text.size()).empty(), "An empty side must copy no text");
    check(added.rowCount(true) == 3 && removed.rowCount(true) == 2,
        "Inline additions and deletions must omit empty alignment cells");
}
void inlineLayout() {
    const auto diff = parse("@@ -10,4 +20,5 @@\n context\n-old1\n-old2\n+new1\n+new2\n+new3\n tail\n");
    check(diff.rowCount(false) == 5 && diff.rowCount(true) == 7, "Inline replacements must display both removed and added lines");
    const std::vector<std::string> expected{"context", "old1", "old2", "new1", "new2", "new3", "tail"};
    for (size_t i = 0; i < diff.rowCount(true); ++i) {
        const auto row = diff.displayRow(i, true);
        const auto source = row.after.number ? row.after : row.before;
        check(diff.line(source) == expected[i], "Inline changes must list all removals before all additions");
        if (i == 0 || i == 6) check(!row.changed && row.before.number && row.after.number, "Inline context must retain both line numbers and appear once");
        else if (i < 3) check(row.changed && row.before.number == 10 + i && !row.after.number, "Inline removals must show only the original line number");
        else check(row.changed && !row.before.number && row.after.number == 18 + i, "Inline additions must show only the new line number");
        for (const bool after : {false, true}) {
            const auto line = after ? row.after : row.before;
            if (!line.number) continue;
            check(diff.rowForLine(line, after, true) == i, "Switching to inline must find the same source line");
            const auto split = diff.displayRow(diff.rowForLine(line, after, false), false);
            check((after ? split.after : split.before).row == line.row, "Switching to split must find the same source line");
        }
    }
    check(diff.selectedText(false, 0, diff.patch.text.size(), true) == "context\nold1\nold2\nnew1\nnew2\nnew3\ntail\n",
        "Inline copy must include both kinds of changes and omit metadata and prefixes");
    const auto oldLine = diff.displayRow(1, true).before, newLine = diff.displayRow(4, true).after;
    check(diff.selectedText(true, diff.offset(oldLine) + 1, diff.offset(newLine) + 3, true) == "ld1\nold2\nnew1\nnew",
        "An inline selection may span removed and added text");
    check(diff.selectedText(false, diff.offset(newLine) + 3, diff.offset(oldLine) + 1, true) == "ld1\nold2\nnew1\nnew",
        "Backward inline selections must copy the same mixed text");
    const auto hunks = parse("@@ -1 +1 @@\n-old1\n+new1\n@@ -10 +10 @@\n-old2\n+new2\n");
    check(hunks.selectedText(false, 0, hunks.patch.text.size(), true) == "old1\nnew1\nold2\nnew2\n",
        "Inline ordering must preserve separate hunks without context rows");
}
void edgeCases() {
    const auto crlf = parse("@@ -2 +2 @@\r\n-\tあ\r\n+\tい\r\n\\ No newline at end of file\r\n");
    check(crlf.rows.size() == 1 && crlf.line(crlf.rows[0].before) == "\tあ" && crlf.maxColumns == 6,
        "CRLF, tabs and wide characters must use source display widths");
    check(crlf.selectedText(true, 0, crlf.patch.text.size()) == "\tい", "No-newline markers must not be copied");
    check(crlf.selectedText(false, 0, crlf.patch.text.size()) == "\tあ\n", "Each side must retain its own final newline");
    const auto noNewlines = parse("@@ -1 +1 @@\n-old\n\\ No newline at end of file\n+new\n\\ No newline at end of file\n");
    check(noNewlines.rows.size() == 1 && noNewlines.selectedText(false, 0, noNewlines.patch.text.size()) == "old" &&
        noNewlines.selectedText(true, 0, noNewlines.patch.text.size()) == "new", "Newline markers must not split a replacement group");
    check(noNewlines.selectedText(false, 0, noNewlines.patch.text.size(), true) == "old\nnew",
        "Inline copy must separate displayed lines even when both sources lack a final newline");
    for (const auto* patch : {"", "diff --git a/a b/b\nold mode 100644\nnew mode 100755\n",
        "diff --git a/a b/b\nBinary files a/a and b/b differ\n", "@@@ -1,1 -1,1 +1,1 @@@\n--old\n++new\n",
        "@@ -4294967296 +1 @@\n-old\n+new\n", "@@ -0 +1 @@\n-old\n+new\n"})
        check(parse(patch).rows.empty(), "Metadata-only, binary and unsupported or invalid hunks must not become source text");
    auto reused = parse("@@ -1 +1 @@\n-a\n+b\n");
    reused.patch.text.clear(); indexDiff(reused);
    check(reused.rows.empty() && reused.inlineOrder.empty() && reused.maxColumns == 0, "Reindexing must discard old rows and widths");
}
void viewPositions() {
    const auto diff = parse("@@ -10,4 +20,5 @@\n context\n-old1\n-old2\n+new1\n+new2\n+new3\n tail\n");
    for (const bool after : {false, true}) {
        for (size_t row = 0; row < diff.rowCount(true); ++row)
            check(diff.mapRow(row, true, true, after) == row, "Saved inline positions must retain the exact row independently of the selection side");
        for (size_t row = 0; row < diff.rowCount(false); ++row)
            check(diff.mapRow(diff.mapRow(row, false, true, after), true, false, after) == row,
                "Saving split positions in inline coordinates must preserve their source rows");
    }
    check(diff.mapRow(6, true, false, false) == 4 && diff.mapRow(4, false, true, false) == 6,
        "A tab's context position must survive changing the project layout in another tab");
    check(diff.mapRow(1, false, true, false) == 1 && diff.mapRow(1, false, true, true) == 3,
        "Restoring a replacement must retain the selected side");
    check(diff.mapRow(5, true, false, false) == 3 && diff.mapRow(3, false, true, false) == 5,
        "Positions on unpaired additions must fall back to the available side");
    const auto deleted = parse("@@ -1,2 +0,0 @@\n-first\n-second\n");
    check(deleted.mapRow(1, false, true, true) == 1, "Positions in deleted files must fall back to the original side");
    check(diff.mapRow(100, false, true, false) == 6 && diff.mapRow(100, true, false, true) == 4 &&
          diff.mapRow(100, false, false, true) == 4 && parse("").mapRow(100, false, true, false) == 0,
        "Restoring positions after a diff shrinks must stay within the document");
}
void wrapping() {
    const auto diff = parse("@@ -1,3 +1,3 @@\n abcdefghij\n-abcdefghijkl\n+x\n tail\n");
    const auto original = diff.patch.text;
    DiffLayout layout; layout.build(diff, false, 4);
    check(layout.rows() == 7, "Split diff height must use the taller replacement side");
    check(layout.row(4).row == 1 && layout.row(4).continuation == 1 && layout.row(6).row == 2,
        "Wrapped replacements must keep the following context aligned");
    const auto before = layout.textRow(diff, 4, false), padding = layout.textRow(diff, 4, true);
    check(before.begin == 4 && before.end == 8 && before.column == 4,
        "Diff continuations must use source bytes without patch prefixes");
    check(padding.begin == 1 && padding.end == 1, "A shorter split side must have empty padding without repeating its text");
    check(layout.rowForOffset(diff, 1, 8, false) == 5 && layout.rowForOffset(diff, 1, 1, true) == 3,
        "Split search and scroll positions must use each side's own wrap boundaries");
    layout.build(diff, true, 4);
    check(layout.rows() == 8 && layout.row(6).row == 2 && layout.textRow(diff, 6, true).end == 1,
        "Inline diff must wrap removals and additions independently");
    check(layout.rowForOffset(diff, 1, 4, false) == 4 && layout.rowForOffset(diff, 2, 0, true) == 6,
        "Inline positions must preserve the source side and continuation");
    check(diff.patch.text == original && diff.selectedText(false, diff.offset(diff.rows[1].before) + 2,
        diff.offset(diff.rows[1].before) + 10) == "cdefghij", "Wrapping must not insert newlines into copied source text");

    const auto unicode = parse(u8"@@ -1 +1 @@\r\n-a\t日本😀z\r\n+abc\tい\r\n");
    layout.build(unicode, false, 4);
    check(layout.rows() == 3, "Tabs and Unicode must use source display widths");
    const auto text = layout.textRow(unicode, 1, false);
    check(text.begin == 2 && text.end == 8 && text.column == 4, "Wrapping split UTF-8 or changed tab stops");
    const auto last = layout.textRow(unicode, 2, false);
    check(last.begin == 8 && last.end == 13 && last.column == 8, "Emoji continuation byte boundaries are wrong");
    for (int columns : {1, 2, 3, 4, 7, 80}) for (bool inlineView : {false, true}) {
        layout.build(unicode, inlineView, columns);
        for (size_t i = 0; i < layout.rows(); ++i) {
            const auto row = layout.row(i); const auto change = unicode.displayRow(row.row, inlineView);
            for (bool after : {false, true}) {
                const auto source = after ? change.after : change.before;
                const auto segment = layout.textRow(unicode, i, after);
                if (!source.number || segment.begin == segment.end) continue;
                check(layout.rowForOffset(unicode, row.row, segment.begin, after) == i, "Unicode diff positions failed to round-trip");
                check((static_cast<unsigned char>(unicode.line(source)[segment.begin]) & 0xc0) != 0x80, "Diff row starts inside a UTF-8 character");
            }
        }
    }
    const auto added = parse("@@ -0,0 +1,2 @@\n+abcdefgh\n+\n");
    layout.build(added, false, 3);
    check(layout.rows() == 4 && layout.textRow(added, 1, false).end == 0 && layout.row(3).row == 1,
        "Unpaired additions or empty source lines lost their alignment");
    auto headings = parse("diff --git a/long-file b/long-file\n@@ -1 +1 @@\n-abcdefgh\n+x\n");
    indexDiff(headings, true); layout.build(headings, false, 4);
    check(layout.rows() == 3 && headings.displayRow(layout.row(0).row, false).heading && layout.row(1).row == 1,
        "File headings must remain separate from wrapped source rows");
    layout.build(parse(""), true, 4); check(layout.rows() == 0, "An empty diff retained wrapped rows");
    const auto longLine = parse("@@ -1 +1 @@\n-" + std::string(maxLineColumns + 100, 'x') + "\n+x\n");
    layout.build(longLine, false, 1);
    check(layout.rows() == maxLineColumns && layout.textRow(longLine, maxLineColumns - 1, false).end == maxLineColumns,
        "Diff wrapping must retain the source display length limit");
}
}
int main() {
    try { syntaxHighlighting(); multilineSyntax(); syntaxLanguages(); alignment(); additionsAndDeletions(); inlineLayout(); edgeCases(); viewPositions(); wrapping(); std::cout << "Diff tests passed.\n"; return EXIT_SUCCESS; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return EXIT_FAILURE; }
}
