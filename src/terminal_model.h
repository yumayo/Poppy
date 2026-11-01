#pragma once
#include "text.h"
#include <array>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace poppy {
using Color = uint32_t; // 0x00BBGGRR, also COLORREF on Windows.
constexpr Color rgb(unsigned r, unsigned g, unsigned b) { return r | (g << 8) | (b << 16); }
constexpr Color foreground = rgb(211, 218, 227), background = rgb(22, 25, 31);
struct Cell {
    char32_t ch = U' ';
    Color fg = foreground, bg = background;
    bool continuation = false;
};
class TerminalModel {
public:
    TerminalModel(int columns = 80, int rows = 24);
    void feed(std::string_view bytes);
    void resize(int columns, int rows);
    const std::vector<Cell>& row(int y, size_t scroll = 0) const;
    size_t historySize() const { return alternate_ ? 0 : history_.size(); }
    // Stable row coordinates even when the bounded history drops old lines.
    int64_t firstRow() const { return alternate_ ? 0 : firstRow_; }
    uint64_t generation() const { return generation_; }
    int columns() const { return columns_; }
    int rows() const { return rows_; }
    int cursorX() const { return active().x; }
    int cursorY() const { return active().y; }
    bool wrapPending() const { return active().wrap; }
    // A nonzero length joins this physical row to the next by automatic wrap.
    // A wide glyph can wrap with one unused cell at the right edge.
    int wrappedLength(int y) const { return active().wrapped.at(static_cast<size_t>(y)); }
    bool cursorVisible() const { return cursorVisible_; }
    bool bracketedPaste() const { return bracketedPaste_; }
    bool applicationCursor() const { return applicationCursor_; }
    bool alternate() const { return alternate_; }
    bool synchronizedOutput() const { return synchronizedOutput_; }
    const std::string& title() const { return title_; }
    void endSynchronizedOutput() { synchronizedOutput_ = false; }
    std::function<void(std::string)> reply;
private:
    struct Screen { std::vector<std::vector<Cell>> cells; std::vector<int> wrapped; int x = 0, y = 0, savedX = 0, savedY = 0; bool wrap = false; };
    enum class Parse { Text, Escape, Csi, CsiDiscard, Osc, OscEscape, Discard, DiscardEscape, Charset };
    Screen& active() { return alternate_ ? alternateScreen_ : normal_; }
    const Screen& active() const { return alternate_ ? alternateScreen_ : normal_; }
    void reset();
    void put(char32_t c);
    void newline(bool wrapped = false);
    void scrollUp(int count);
    void scrollDown(int count);
    void csi(char command);
    void osc();
    void sgr(const std::vector<int>& args);
    Cell blank() const;
    void erase(int y, int begin, int end);
    void repair(int y);
    int columns_, rows_, top_ = 0, bottom_;
    Screen normal_, alternateScreen_;
    std::deque<std::vector<Cell>> history_;
    size_t historyCells_ = 0;
    int64_t firstRow_ = 0;
    uint64_t generation_ = 0;
    Color fg_ = foreground, bg_ = background;
    bool inverse_ = false, bold_ = false, alternate_ = false, cursorVisible_ = true;
    bool bracketedPaste_ = false, applicationCursor_ = false, autoWrap_ = true, origin_ = false;
    bool synchronizedOutput_ = false;
    Parse parse_ = Parse::Text;
    std::string sequence_, utf8_, title_;
    bool oscOverflow_ = false;
};
}
