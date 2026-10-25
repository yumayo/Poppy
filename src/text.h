#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace poppy {
enum class Language { Plain, CLike, Python, Shell, Json, Config, Diff };
enum class Style : uint8_t { Text, Keyword, String, Comment, Number, Removed };
enum class LexState : uint8_t { Normal, BlockComment, TripleSingle, TripleDouble, Backtick };
struct Token { size_t begin, end; Style style; };
struct Document {
    std::string text;
    std::vector<uint32_t> lines;
    std::vector<LexState> states;
    Language language = Language::Plain;
    std::string encoding = "UTF-8";
    uint32_t maxColumns = 0;
    std::string_view line(size_t row) const;
};
constexpr size_t maxFileBytes = 16 * 1024 * 1024;
constexpr size_t maxLineColumns = 8192;
// Sparse UTF-8 checkpoints let a visible line start near the horizontal scroll
// position without decoding its entire prefix on every repaint.
class LineIndex {
public:
    struct Position { uint32_t byte = 0, column = 0; };
    static constexpr uint32_t stride = 64;
    void build(std::string_view line, Language language, LexState state);
    Position before(uint32_t column) const;
    const std::vector<Token>& tokens() const { return tokens_; }
    size_t bytes() const { return positions_.capacity() * sizeof(Position) + tokens_.capacity() * sizeof(Token); }
private:
    std::vector<Position> positions_;
    std::vector<Token> tokens_;
};
Language languageFor(std::string extension);
bool decodeText(std::string bytes, std::string& text, std::string& encoding, std::string& error);
void indexDocument(Document& document);
void lexLine(std::string_view line, Language language, LexState& state, std::vector<Token>* tokens = nullptr);
// Invalid UTF-8 consumes one byte and yields U+FFFD. A truncated sequence consumes zero bytes.
char32_t decodeUtf8(std::string_view text, size_t& position, bool final = true);
void appendUtf8(std::string& text, char32_t codepoint);
int cellWidth(char32_t codepoint);
}
