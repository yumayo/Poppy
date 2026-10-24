#include "diff.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>

using namespace poppy;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
DiffDocument parse(std::string patch) {
    DiffDocument result; result.patch.text = std::move(patch); indexDiff(result); return result;
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
}
int main() {
    try { alignment(); additionsAndDeletions(); inlineLayout(); edgeCases(); viewPositions(); std::cout << "Diff tests passed.\n"; return EXIT_SUCCESS; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return EXIT_FAILURE; }
}
