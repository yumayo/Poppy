#include "csv_table.h"
#include "edit_buffer.h"
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

using namespace poppy;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
EditBuffer load(std::string source) {
    EditBuffer buffer; std::string error;
    check(decodeText(source, buffer.document.text, buffer.document.encoding, error), "Cannot decode CSV fixture");
    indexDocument(buffer.document); buffer.initialize(std::move(source)); return buffer;
}
std::string apply(std::string source, const std::optional<CsvReplacement>& change) {
    check(change.has_value(), "CSV edit was rejected");
    source.replace(change->begin, change->end - change->begin, change->text); return source;
}
void parsing() {
    for (auto extension : {L".csv", L".CSV", L".cSv"}) check(isCsvExtension(extension), "CSV extension rejected");
    for (auto extension : {L"csv", L".tsv", L".csv.txt", L"", L".html"}) check(!isCsvExtension(extension), "Non-CSV extension accepted");
    check(csvColumnName(0) == "A" && csvColumnName(25) == "Z" && csvColumnName(26) == "AA" && csvColumnName(701) == "ZZ" &&
        csvColumnName(702) == "AAA" && csvColumnName(16383) == "XFD", "Column labels are incorrect");
    const std::string source = u8"名前,数量,メモ,空\r\n豆腐,001,\"a,b\",\r\n\"改行\r\nあり\",-2,\"say \"\"hello\"\"\",\"\"\nshort\r\n";
    CsvTable table; check(table.parse(source), "Valid CSV rejected");
    check(table.rows() == 4 && table.columns() == 4 && table.fields(3) == 1, "Ragged rows or final newline changed the table dimensions");
    check(table.value(source, {1, 1}) == "001" && table.value(source, {1, 2}) == "a,b", "CSV values were converted or split");
    check(table.value(source, {2, 0}) == u8"改行\r\nあり" && table.value(source, {2, 2}) == "say \"hello\"", "Quoted data was not decoded");
    check(table.value(source, {1, 3}).empty() && table.value(source, {3, 2}).empty(), "Missing and empty cells are not empty");
    for (size_t r = 0; r < table.rows(); ++r) for (size_t c = 0; c < table.fields(r); ++c) {
        const auto position = table.position(table.offset({r, c}));
        check(position.row == r && position.column == c, "Field byte offset did not round trip");
    }
    check(table.position(source.find(u8"あり")).row == 2, "Embedded newline became a separate record");
    for (const auto& text : {std::string{}, std::string("\n"), std::string("\r\n"), std::string("\"\""), std::string("value\n")}) {
        check(table.parse(text) && table.rows() == 1 && table.columns() == 1, "Single empty/terminated record has incorrect dimensions");
    }
    check(table.parse("a,b,\r\n\n,,") && table.rows() == 3 && table.columns() == 3, "Empty records and trailing fields changed");
    for (const auto& text : {"\"unclosed", "a,b\"c", "\"x\"tail,y", "\"\"\""}) {
        check(!table.parse(text) && !table.error().empty() && table.rows() == 0, "Malformed CSV was silently accepted");
        check(!table.replace(text, {}, {{"x"}}), "Malformed CSV was editable");
    }
    check(table.parse("a,b") && table.error().empty(), "Parser did not recover after an invalid file");
    const std::string tooWide(CsvTable::maxColumns, ',');
    check(!table.parse(tooWide), "Excessive CSV columns were not bounded");
    const std::string tooManyCells(CsvTable::maxCells, '\n');
    check(table.parse(tooManyCells) && table.rows() == CsvTable::maxCells, "Exact table limit rejected");
    check(!table.parse(tooManyCells + '\n'), "Excessive CSV cells were not bounded");
}
void edits() {
    const std::string original = "\"keep\",001,\"old\"\r\nlast\nend,,\r\n";
    CsvTable table; check(table.parse(original), "Cannot parse editing fixture");
    auto edited = apply(original, table.replace(original, {0, 2}, {{"new,\"value\"\nline"}}));
    check(edited == "\"keep\",001,\"new,\"\"value\"\"\nline\"\r\nlast\nend,,\r\n", "Editing normalized unrelated bytes or failed to quote a cell");
    check(apply(original, table.replace(original, {0, 2}, {{"old"}})) == original, "Editing an unchanged value removed its original quoting");
    edited = apply(original, table.replace(original, {1, 1}, {{"A", "B"}, {"C", "D"}}));
    check(edited == "\"keep\",001,\"old\"\r\nlast,A,B\nend,C,D\r\n", "Rectangular paste changed unrelated fields or separators");
    edited = apply(original, table.clear(original, {0, 1, 2, 2}));
    check(edited == "\"keep\",,\r\nlast\nend,,\r\n", "Clear expanded ragged records or changed other cells");
    for (const auto& source : {std::string("a,b"), std::string("a,b\n"), std::string("a,b\r\n")}) {
        check(table.parse(source), "Cannot parse append fixture");
        const auto newline = source.find('\r') != std::string::npos ? "\r\n" : "\n";
        edited = apply(source, table.replace(source, {1, 2}, {{"tail"}}));
        check(edited == std::string("a,b") + newline + ",,tail" + (source.back() == '\n' ? newline : ""), "Appending lost the terminal newline convention");
        check(table.parse(edited) && table.rows() == 2 && table.value(edited, {1, 2}) == "tail", "Appended record did not round trip");
    }
    check(table.parse("a\nb"), "Cannot parse clear fixture");
    edited = apply("a\nb", table.clear("a\nb", {0, 0, 1, 0}));
    check(table.parse(edited) && table.rows() == 2 && table.value(edited, {1, 0}).empty(), "Clearing a single-column final record removed the row");
    check(table.parse(""), "Cannot parse empty CSV");
    edited = apply("", table.replace("", {2, 1}, {{u8"日本語😀"}}));
    check(table.parse(edited) && table.rows() == 3 && table.columns() == 2 && table.value(edited, {2, 1}) == u8"日本語😀", "Appending into an empty file lost blank rows");
    check(!table.replace(edited, {0, CsvTable::maxColumns}, {{"bad"}}) && !table.replace(edited, {CsvTable::maxCells, 0}, {{"bad"}}), "Out-of-range paste accepted");
    check(!table.replace(edited, {std::numeric_limits<size_t>::max(), 0}, {{"bad"}}), "Paste coordinates overflowed");
    check(!table.copy(edited, {0, 0, CsvTable::maxCells - 1, CsvTable::maxColumns - 1}), "Excessive rectangular copy was not bounded");
}
void clipboard() {
    const std::string source = "\"a,b\",\"tab\there\",\"first\r\nsecond\"\r\n\"\"\"quote\"\"\",001,\"\"\r\n";
    CsvTable table; check(table.parse(source), "Cannot parse clipboard fixture");
    const auto copied = table.copy(source, {0, 0, 1, 2}); check(copied.has_value(), "Cannot copy CSV rectangle");
    check(copied->find("a,b\t\"tab\there\"\t\"first\r\nsecond\"") == 0, "Clipboard is not Excel-compatible TSV");
    CsvTable clipboard; check(clipboard.parse(*copied, '\t'), "Cannot parse Excel clipboard text");
    check(clipboard.values(*copied) == table.values(source), "Copy/paste changed tabs, newlines, quotes or leading zeros");
    check(table.parse("\"\"\n\"\""), "Cannot parse empty clipboard cells");
    const auto empty = table.copy("\"\"\n\"\"", {0, 0, 1, 0});
    check(empty && clipboard.parse(*empty, '\t') && clipboard.rows() == 2 && clipboard.columns() == 1, "Copying empty rows lost a record");
    const std::vector<std::string> values{"", "plain", "a,b", "\"quoted\"", "\n", "\r\n", "\t", u8"日本語😀", "001", "=SUM(A1:A2)"};
    for (const char delimiter : {',', '\t'}) for (const auto& a : values) for (const auto& b : values) {
        const auto record = csvQuote(a, delimiter) + delimiter + csvQuote(b, delimiter);
        check(clipboard.parse(record, delimiter) && clipboard.value(record, {0, 0}) == a && clipboard.value(record, {0, 1}) == b, "Quoted values did not round trip");
    }
}
void historyAndEncoding() {
    const std::string source = "\xef\xbb\xbf\"keep\",001\r\n";
    auto buffer = load(source); CsvTable table; check(table.parse(buffer.document.text), "Cannot parse BOM fixture");
    for (const auto& value : {"X", "XY", "XYZ"}) {
        const auto change = table.replace(buffer.document.text, {0, 1}, {{value}}); check(change.has_value(), "Cannot edit cell");
        check(buffer.replace(change->begin, change->end, change->text, std::string(value) != "X"), "Edit buffer rejected CSV replacement");
        check(table.parse(buffer.document.text), "Editing broke CSV syntax");
    }
    check(buffer.dirty() && buffer.encoded() == "\xef\xbb\xbf\"keep\",XYZ\r\n", "CSV editing changed the BOM or newline");
    const auto edited = buffer.encoded(); size_t anchor = 0, caret = 0;
    check(buffer.undo(anchor, caret) && !buffer.dirty() && buffer.encoded() == source && !buffer.undo(anchor, caret), "A cell edit was not one undo operation");
    check(buffer.redo(anchor, caret) && buffer.encoded() == edited, "Cannot redo a grouped cell edit");
    // A new cell edit must remain a distinct operation, even in the same row.
    check(table.parse(buffer.document.text), "Cannot reparse redone CSV");
    const auto change = table.replace(buffer.document.text, {0, 0}, {{"another"}});
    check(change && buffer.replace(change->begin, change->end, change->text), "Cannot edit another cell");
    check(buffer.undo(anchor, caret) && buffer.encoded() == edited, "Separate cell edits were merged");
    auto appended = load("a,b");
    for (const auto& value : {"X", "XY", "XYZ"}) {
        check(table.parse(appended.document.text), "Cannot parse appended CSV");
        const auto row = table.replace(appended.document.text, {1, 0}, {{value}});
        check(row && appended.replace(row->begin, row->end, row->text, std::string(value) != "X"), "Cannot type in an appended row");
    }
    check(appended.document.text == "a,b\nXYZ" && appended.undo(anchor, caret) && appended.document.text == "a,b" && !appended.undo(anchor, caret),
        "Typing in a new row was not one undo operation");
    auto utf16 = load(std::string("\xff\xfe") + std::string("a\0,\0b\0\r\0\n\0", 10));
    check(table.parse(utf16.document.text), "Cannot parse UTF-16 CSV");
    const auto unicode = table.replace(utf16.document.text, {0, 1}, {{u8"豆腐"}});
    check(unicode && utf16.replace(unicode->begin, unicode->end, unicode->text), "Cannot edit UTF-16 CSV");
    auto reloaded = load(utf16.encoded());
    check(reloaded.document.encoding == "UTF-16 LE" && reloaded.document.text == u8"a,豆腐\r\n", "Saving changed the CSV encoding");
}
void rectangularEdits() {
    std::mt19937 random(79421);
    const std::vector<std::string> samples{"", "001", "a,b", "\"x\"", "one\ntwo", "\r\n", u8"日本語😀"};
    for (int iteration = 0; iteration < 300; ++iteration) {
        CsvValues expected(1 + random() % 5); std::string source;
        for (auto& row : expected) {
            row.resize(1 + random() % 4);
            if (!source.empty()) source += random() % 2 ? "\r\n" : "\n";
            for (size_t c = 0; c < row.size(); ++c) {
                row[c] = samples[random() % samples.size()];
                if (c) source += ',';
                source += row.size() == 1 && row[c].empty() ? "\"\"" : csvQuote(row[c]);
            }
        }
        if (random() % 2) source += "\r\n";
        CsvTable table; check(table.parse(source), "Generated CSV fixture is invalid");
        const CsvPosition first{random() % 7, random() % 5};
        CsvValues incoming(1 + random() % 3, std::vector<std::string>(1 + random() % 3));
        for (auto& row : incoming) for (auto& value : row) value = samples[random() % samples.size()];
        const auto edited = apply(source, table.replace(source, first, incoming));
        expected.resize(std::max(expected.size(), first.row + incoming.size()), {""});
        for (size_t r = 0; r < incoming.size(); ++r) {
            auto& row = expected[first.row + r]; row.resize(std::max(row.size(), first.column + incoming[r].size()));
            for (size_t c = 0; c < incoming[r].size(); ++c) row[first.column + c] = incoming[r][c];
        }
        check(table.parse(edited) && table.values(edited) == expected, "Rectangular edit changed data outside its destination");
        const auto cleared = apply(edited, table.clear(edited, {first.row, first.column, first.row + incoming.size() - 1, first.column + incoming[0].size() - 1}));
        for (size_t r = 0; r < incoming.size(); ++r) for (size_t c = 0; c < incoming[r].size(); ++c) expected[first.row + r][first.column + c].clear();
        check(table.parse(cleared) && table.values(cleared) == expected, "Clearing a rectangle changed record dimensions or other values");
    }
}
}
int main() {
    try { parsing(); edits(); clipboard(); historyAndEncoding(); rectangularEdits(); std::cout << "CSV tests passed\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
