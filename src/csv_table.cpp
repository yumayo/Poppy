#include "csv_table.h"
#include "text.h"
#include <algorithm>

namespace poppy {
bool isCsvExtension(std::wstring_view extension) {
    return extension.size() == 4 && extension[0] == L'.' &&
        (extension[1] == L'c' || extension[1] == L'C') &&
        (extension[2] == L's' || extension[2] == L'S') &&
        (extension[3] == L'v' || extension[3] == L'V');
}
std::string csvColumnName(size_t column) {
    std::string name;
    do {
        name += static_cast<char>('A' + column % 26);
        if (column < 26) break;
        column = column / 26 - 1;
    } while (true);
    std::reverse(name.begin(), name.end()); return name;
}
std::string csvQuote(std::string_view value, char delimiter) {
    if (value.find(delimiter) == std::string_view::npos && value.find_first_of("\"\r\n") == std::string_view::npos) return std::string(value);
    std::string result = "\"";
    for (const auto ch : value) { result += ch; if (ch == '"') result += '"'; }
    result += '"'; return result;
}
bool CsvTable::parse(std::string_view source, char delimiter) {
    fields_.clear(); rows_.clear(); columns_ = 0; delimiter_ = delimiter; error_.clear();
    const auto fail = [&](const char* message, size_t offset) {
        error_ = std::string(message) + " (byte " + std::to_string(offset + 1) + ")";
        fields_.clear(); rows_.clear(); columns_ = 0; return false;
    };
    if (source.size() > maxFileBytes) return fail("CSV exceeds the file size limit", 0);
    size_t p = 0;
    do {
        Row row{static_cast<uint32_t>(fields_.size()), 0, static_cast<uint32_t>(p), 0, 0};
        while (true) {
            const size_t begin = p;
            if (p < source.size() && source[p] == '"') {
                ++p; bool closed = false;
                while (p < source.size()) {
                    if (source[p++] != '"') continue;
                    if (p < source.size() && source[p] == '"') { ++p; continue; }
                    closed = true; break;
                }
                if (!closed) return fail("Unclosed quoted field", begin);
                if (p < source.size() && source[p] != delimiter && source[p] != '\r' && source[p] != '\n')
                    return fail("Unexpected character after a quoted field", p);
            } else {
                while (p < source.size() && source[p] != delimiter && source[p] != '\r' && source[p] != '\n') {
                    if (source[p] == '"') return fail("Quote inside an unquoted field", p);
                    ++p;
                }
            }
            if (fields_.size() >= maxCells || row.count >= maxColumns) return fail("CSV exceeds the table size limit; use Source", p);
            fields_.push_back({static_cast<uint32_t>(begin), static_cast<uint32_t>(p)}); ++row.count;
            if (p == source.size() || source[p] != delimiter) break;
            ++p;
        }
        row.end = static_cast<uint32_t>(p);
        if (p < source.size() && source[p++] == '\r' && p < source.size() && source[p] == '\n') ++p;
        row.next = static_cast<uint32_t>(p);
        rows_.push_back(row); columns_ = std::max(columns_, static_cast<size_t>(row.count));
    } while (p < source.size());
    return true;
}
size_t CsvTable::fields(size_t row) const { return row < rows_.size() ? rows_[row].count : 0; }
size_t CsvTable::offset(CsvPosition cell) const {
    if (cell.row >= rows_.size()) return rows_.empty() ? 0 : rows_.back().next;
    const auto& row = rows_[cell.row];
    return cell.column < row.count ? fields_[row.first + cell.column].begin : row.end;
}
CsvPosition CsvTable::position(size_t offset) const {
    if (rows_.empty()) return {};
    auto row = std::upper_bound(rows_.begin(), rows_.end(), offset, [](size_t p, const Row& row) { return p < row.begin; });
    if (row != rows_.begin()) --row;
    const auto first = fields_.begin() + row->first;
    auto field = std::upper_bound(first, first + row->count, offset, [](size_t p, const Field& field) { return p < field.begin; });
    if (field != first) --field;
    return {static_cast<size_t>(row - rows_.begin()), static_cast<size_t>(field - first)};
}
std::string CsvTable::value(std::string_view source, CsvPosition cell) const {
    if (cell.row >= rows_.size() || cell.column >= rows_[cell.row].count) return {};
    const auto field = fields_[rows_[cell.row].first + cell.column];
    auto raw = source.substr(field.begin, field.end - field.begin);
    if (raw.empty() || raw.front() != '"') return std::string(raw);
    raw.remove_prefix(1); raw.remove_suffix(1);
    std::string result; result.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) { result += raw[i]; if (raw[i] == '"') ++i; }
    return result;
}
CsvValues CsvTable::values(std::string_view source) const {
    CsvValues result; result.reserve(rows());
    for (size_t r = 0; r < rows(); ++r) {
        result.emplace_back(); result.back().reserve(fields(r));
        for (size_t c = 0; c < fields(r); ++c) result.back().push_back(value(source, {r, c}));
    }
    return result;
}
std::optional<CsvReplacement> CsvTable::replace(std::string_view source, CsvPosition first, const CsvValues& values) const {
    if (values.empty() || first.row > maxCells - std::min(maxCells, values.size())) return {};
    size_t columns = 0;
    for (const auto& row : values) columns = std::max(columns, row.size());
    if (!columns || columns > maxColumns || first.column > maxColumns - columns) return {};
    return change(source, {first.row, first.column, first.row + values.size() - 1, first.column + columns - 1}, &values);
}
std::optional<CsvReplacement> CsvTable::clear(std::string_view source, CsvRange range) const {
    if (rows_.empty() || !columns_ || range.firstRow >= rows() || range.firstColumn >= columns_) return {};
    range.lastRow = std::min(range.lastRow, rows() - 1); range.lastColumn = std::min(range.lastColumn, columns_ - 1);
    return change(source, range, nullptr);
}
std::optional<CsvReplacement> CsvTable::change(std::string_view source, CsvRange range, const CsvValues* values) const {
    if (!error_.empty() || rows_.empty() || range.lastRow < range.firstRow || range.lastColumn < range.firstColumn ||
        range.lastRow >= maxCells || range.lastColumn >= maxColumns) return {};
    const auto& last = rows_.back();
    const std::string newline = [&] {
        for (const auto& row : rows_) if (row.next > row.end) return std::string(source.substr(row.end, row.next - row.end));
        return std::string("\n");
    }();
    const size_t firstRow = std::min(range.firstRow, rows());
    const size_t begin = firstRow < rows() ? rows_[firstRow].begin : source.size();
    const size_t end = range.lastRow < rows() ? rows_[range.lastRow].end : source.size();
    std::string text;
    if (firstRow == rows() && last.next == last.end) text += newline;
    size_t cells = firstRow < rows() ? rows_[firstRow].first : fields_.size();
    for (size_t r = firstRow; r <= range.lastRow; ++r) {
        const auto* incoming = values && r >= range.firstRow ? &(*values)[r - range.firstRow] : nullptr;
        const size_t count = std::max(fields(r), incoming ? range.firstColumn + incoming->size() : size_t{1});
        if (count > maxCells - std::min(maxCells, cells)) return {};
        cells += count;
        for (size_t c = 0; c < count; ++c) {
            if (c) text += delimiter_;
            const bool overwrite = r >= range.firstRow && c >= range.firstColumn && c <= range.lastColumn &&
                (!values || (incoming && c - range.firstColumn < incoming->size()));
            const std::string* value = overwrite && values ? &(*incoming)[c - range.firstColumn] : nullptr;
            if (c < fields(r) && (!overwrite || (value ? *value == this->value(source, {r, c}) : this->value(source, {r, c}).empty()))) {
                const auto field = fields_[rows_[r].first + c]; text.append(source.substr(field.begin, field.end - field.begin));
            } else if (value) {
                if (value->empty() && count == 1) text += "\"\"";
                else text += csvQuote(*value, delimiter_);
            } else if (count == 1) text += "\"\"";
            if (text.size() > maxFileBytes) return {};
        }
        if (r < range.lastRow) {
            if (r < rows() && rows_[r].next > rows_[r].end) text.append(source.substr(rows_[r].end, rows_[r].next - rows_[r].end));
            else text += newline;
        }
    }
    if (range.lastRow >= rows() && last.next > last.end) text.append(source.substr(last.end, last.next - last.end));
    if (range.lastRow + 1 < rows()) cells += fields_.size() - rows_[range.lastRow + 1].first;
    if (cells > maxCells || source.size() - (end - begin) + text.size() > maxFileBytes) return {};
    return CsvReplacement{begin, end, std::move(text)};
}
std::optional<std::string> CsvTable::copy(std::string_view source, CsvRange range) const {
    if (range.lastRow < range.firstRow || range.lastColumn < range.firstColumn || range.lastRow >= maxCells || range.lastColumn >= maxColumns ||
        range.lastRow - range.firstRow + 1 > maxCells / (range.lastColumn - range.firstColumn + 1)) return {};
    std::string result;
    for (size_t r = range.firstRow; r <= range.lastRow; ++r) {
        if (r != range.firstRow) result += "\r\n";
        for (size_t c = range.firstColumn; c <= range.lastColumn; ++c) {
            if (c != range.firstColumn) result += '\t';
            const auto text = value(source, {r, c});
            result += text.empty() && range.firstColumn == range.lastColumn ? "\"\"" : csvQuote(text, '\t');
            if (result.size() > maxFileBytes) return {};
        }
    }
    return result;
}
size_t CsvTable::bytes() const { return fields_.capacity() * sizeof(Field) + rows_.capacity() * sizeof(Row); }
}
