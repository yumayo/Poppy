#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace poppy {
bool isCsvExtension(std::wstring_view extension);
std::string csvColumnName(size_t column);
std::string csvQuote(std::string_view value, char delimiter = ',');
struct CsvPosition { size_t row = 0, column = 0; };
struct CsvRange { size_t firstRow, firstColumn, lastRow, lastColumn; };
using CsvValues = std::vector<std::vector<std::string>>;
struct CsvReplacement { size_t begin, end; std::string text; };

// Index raw UTF-8 spans rather than copying every field. Unchanged fields and
// record separators survive edits byte for byte, including ragged records.
class CsvTable {
public:
    static constexpr size_t maxCells = 1000000, maxColumns = 16384;
    bool parse(std::string_view source, char delimiter = ',');
    const std::string& error() const { return error_; }
    size_t rows() const { return rows_.size(); }
    size_t columns() const { return columns_; }
    size_t fields(size_t row) const;
    size_t offset(CsvPosition cell) const;
    CsvPosition position(size_t offset) const;
    std::string value(std::string_view source, CsvPosition cell) const;
    CsvValues values(std::string_view source) const;
    std::optional<CsvReplacement> replace(std::string_view source, CsvPosition first, const CsvValues& values) const;
    std::optional<CsvReplacement> clear(std::string_view source, CsvRange range) const;
    std::optional<std::string> copy(std::string_view source, CsvRange range) const;
    size_t bytes() const;
private:
    struct Field { uint32_t begin, end; };
    struct Row { uint32_t first, count, begin, end, next; };
    std::optional<CsvReplacement> change(std::string_view source, CsvRange range, const CsvValues* values) const;
    std::vector<Field> fields_;
    std::vector<Row> rows_;
    size_t columns_ = 0;
    char delimiter_ = ',';
    std::string error_;
};
}
