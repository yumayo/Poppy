#include "terminal_model.h"
#include <algorithm>
#include <cstdlib>

namespace poppy {
namespace {
constexpr std::array<Color, 16> palette = {
    rgb(22,25,31), rgb(231,112,118), rgb(151,195,121), rgb(224,192,128),
    rgb(115,165,226), rgb(191,151,220), rgb(116,190,194), rgb(211,218,227),
    rgb(98,108,124), rgb(255,141,147), rgb(177,220,147), rgb(249,217,154),
    rgb(148,194,255), rgb(218,179,246), rgb(148,220,223), rgb(245,247,250)
};
Color color256(int n) {
    n = std::clamp(n, 0, 255);
    if (n < 16) return palette[n];
    if (n >= 232) { const auto v = 8 + 10 * (n - 232); return rgb(v,v,v); }
    n -= 16;
    auto level = [](int v) { return v ? 55 + v * 40 : 0; };
    return rgb(level(n / 36), level((n / 6) % 6), level(n % 6));
}
}
TerminalModel::TerminalModel(int columns, int rows) : columns_(std::max(2, columns)), rows_(std::max(1, rows)), bottom_(rows_ - 1) { reset(); }
Cell TerminalModel::blank() const { return {U' ', inverse_ ? bg_ : fg_, inverse_ ? fg_ : bg_, false}; }
void TerminalModel::reset() {
    fg_ = foreground; bg_ = background; inverse_ = bold_ = alternate_ = false;
    cursorVisible_ = autoWrap_ = true; bracketedPaste_ = applicationCursor_ = origin_ = false;
    synchronizedOutput_ = false;
    top_ = 0; bottom_ = rows_ - 1;
    normal_ = {}; alternateScreen_ = {};
    normal_.cells.assign(rows_, std::vector<Cell>(columns_));
    alternateScreen_.cells.assign(rows_, std::vector<Cell>(columns_));
    history_.clear(); historyCells_ = 0;
}
void TerminalModel::resize(int columns, int rows) {
    columns = std::clamp(columns, 2, 500); rows = std::clamp(rows, 1, 200);
    if (columns == columns_ && rows == rows_) return;
    for (Screen* s : {&normal_, &alternateScreen_}) {
        // Keep the cursor visible when the pane shrinks; no unbounded reflow buffer.
        const int remove = std::max(0, s->y - rows + 1);
        if (remove) { s->cells.erase(s->cells.begin(), s->cells.begin() + remove); s->y -= remove; }
        s->cells.resize(rows, std::vector<Cell>(columns));
        for (auto& line : s->cells) {
            line.resize(columns);
            if (!line.empty() && !line.back().continuation && cellWidth(line.back().ch) == 2) line.back() = {};
        }
        s->x = std::clamp(s->x, 0, columns - 1); s->y = std::clamp(s->y, 0, rows - 1); s->wrap = false;
    }
    columns_ = columns; rows_ = rows; top_ = 0; bottom_ = rows - 1;
    // Old history rows keep their original width and are clipped/padded by the renderer.
}
const std::vector<Cell>& TerminalModel::row(int y, size_t scroll) const {
    if (alternate_ || !scroll) return active().cells.at(static_cast<size_t>(y));
    const size_t offset = history_.size() - std::min(scroll, history_.size()) + static_cast<size_t>(y);
    return offset < history_.size() ? history_[offset] : normal_.cells.at(offset - history_.size());
}
void TerminalModel::erase(int y, int begin, int end) {
    auto& line = active().cells[y];
    begin = std::clamp(begin, 0, columns_); end = std::clamp(end, begin, columns_);
    if (begin < columns_ && line[begin].continuation && begin > 0) line[begin - 1] = blank();
    if (end < columns_ && line[end].continuation) line[end] = blank();
    std::fill(line.begin() + begin, line.begin() + end, blank());
}
void TerminalModel::repair(int y) {
    auto& line = active().cells[y];
    for (int x = 0; x < columns_; ++x) {
        if (line[x].continuation && (x == 0 || cellWidth(line[x - 1].ch) != 2)) line[x] = blank();
        else if (!line[x].continuation && cellWidth(line[x].ch) == 2 && (x + 1 == columns_ || !line[x + 1].continuation)) line[x] = blank();
    }
}
void TerminalModel::scrollUp(int count) {
    auto& s = active(); count = std::clamp(count, 1, bottom_ - top_ + 1);
    for (int i = 0; i < count; ++i) {
        if (!alternate_ && top_ == 0 && bottom_ == rows_ - 1) {
            historyCells_ += s.cells.front().size(); history_.push_back(std::move(s.cells.front()));
            // At most 1,000 lines AND 131,072 cells (roughly 2 MiB).
            while (history_.size() > 1000 || historyCells_ > 131072) { historyCells_ -= history_.front().size(); history_.pop_front(); }
        }
        for (int y = top_; y < bottom_; ++y) s.cells[y] = std::move(s.cells[y + 1]);
        s.cells[bottom_] = std::vector<Cell>(columns_, blank());
    }
}
void TerminalModel::scrollDown(int count) {
    auto& s = active(); count = std::clamp(count, 1, bottom_ - top_ + 1);
    for (int i = 0; i < count; ++i) {
        for (int y = bottom_; y > top_; --y) s.cells[y] = std::move(s.cells[y - 1]);
        s.cells[top_] = std::vector<Cell>(columns_, blank());
    }
}
void TerminalModel::newline() {
    auto& s = active(); s.wrap = false;
    if (s.y == bottom_) scrollUp(1); else if (s.y < rows_ - 1) ++s.y;
}
void TerminalModel::put(char32_t c) {
    auto& s = active(); const int width = cellWidth(c);
    // Combining marks/ZWJ composition require a shaping engine; do not consume a cell.
    if (width == 0 || c == 0x200d) return;
    if (s.wrap || (width == 2 && s.x == columns_ - 1)) {
        if (autoWrap_) { s.x = 0; newline(); }
        else s.wrap = false;
    }
    if (width == 2 && s.x + 1 >= columns_) return;
    erase(s.y, s.x, s.x + width);
    auto cell = blank(); cell.ch = c;
    s.cells[s.y][s.x] = cell;
    if (width == 2) { cell.ch = 0; cell.continuation = true; s.cells[s.y][s.x + 1] = cell; }
    if (s.x + width >= columns_) { s.x = columns_ - 1; s.wrap = autoWrap_; }
    else s.x += width;
}
void TerminalModel::sgr(const std::vector<int>& a) {
    for (size_t i = 0; i < a.size(); ++i) {
        const int n = a[i];
        if (n == 0) { fg_ = foreground; bg_ = background; inverse_ = bold_ = false; }
        else if (n == 1) bold_ = true;
        else if (n == 22) bold_ = false;
        else if (n == 7) inverse_ = true;
        else if (n == 27) inverse_ = false;
        else if (n == 39) fg_ = foreground;
        else if (n == 49) bg_ = background;
        else if (n >= 30 && n <= 37) fg_ = palette[n - 30 + (bold_ ? 8 : 0)];
        else if (n >= 40 && n <= 47) bg_ = palette[n - 40];
        else if (n >= 90 && n <= 97) fg_ = palette[n - 90 + 8];
        else if (n >= 100 && n <= 107) bg_ = palette[n - 100 + 8];
        else if ((n == 38 || n == 48) && i + 2 < a.size()) {
            Color color;
            if (a[i + 1] == 5) { color = color256(a[i + 2]); i += 2; }
            else if (a[i + 1] == 2 && i + 4 < a.size()) { color = rgb(std::clamp(a[i + 2], 0, 255), std::clamp(a[i + 3], 0, 255), std::clamp(a[i + 4], 0, 255)); i += 4; }
            else continue;
            (n == 38 ? fg_ : bg_) = color;
        }
    }
}
void TerminalModel::csi(char command) {
    // DECRQM uses an intermediate '$'. Queries must not alter delayed wrapping.
    if (command == 'p' && sequence_ == "?2026$") {
        if (reply) reply(synchronizedOutput_ ? "\x1b[?2026;1$y" : "\x1b[?2026;2$y");
        return;
    }
    const bool priv = !sequence_.empty() && sequence_[0] == '?';
    const bool secondary = !sequence_.empty() && sequence_[0] == '>';
    std::vector<int> args(1, 0);
    for (char c : sequence_) {
        if (c >= '0' && c <= '9') args.back() = std::min(100000, args.back() * 10 + c - '0');
        else if (c == ';' || c == ':') { if (args.size() >= 32) return; args.push_back(0); }
        else if (c != '?' && c != '>') return; // Ignore unsupported intermediate/private sequences.
    }
    auto arg = [&](size_t i, int fallback) { return i < args.size() && args[i] ? args[i] : fallback; };
    auto& s = active();
    const int n = arg(0, 1), minY = origin_ ? top_ : 0, maxY = origin_ ? bottom_ : rows_ - 1;
    const bool synchronizationOnly = priv && (command == 'h' || command == 'l') &&
        std::all_of(args.begin(), args.end(), [](int mode) { return mode == 2026; });
    if (command != 'm' && !synchronizationOnly) s.wrap = false;
    switch (command) {
    case 'A': s.y = std::max(minY, s.y - n); break;
    case 'B': case 'e': s.y = std::min(maxY, s.y + n); break;
    case 'C': case 'a': s.x = std::min(columns_ - 1, s.x + n); break;
    case 'D': s.x = std::max(0, s.x - n); break;
    case 'E': s.y = std::min(maxY, s.y + n); s.x = 0; break;
    case 'F': s.y = std::max(minY, s.y - n); s.x = 0; break;
    case 'G': case '`': s.x = std::clamp(n - 1, 0, columns_ - 1); break;
    case 'd': s.y = std::clamp(minY + n - 1, minY, maxY); break;
    case 'H': case 'f': s.y = std::clamp(minY + n - 1, minY, maxY); s.x = std::clamp(arg(1, 1) - 1, 0, columns_ - 1); break;
    case 'J':
        if (args[0] == 2) for (int y = 0; y < rows_; ++y) erase(y, 0, columns_);
        else if (args[0] == 3) { history_.clear(); historyCells_ = 0; }
        else if (args[0] == 0) { erase(s.y, s.x, columns_); for (int y = s.y + 1; y < rows_; ++y) erase(y, 0, columns_); }
        else if (args[0] == 1) { for (int y = 0; y < s.y; ++y) erase(y, 0, columns_); erase(s.y, 0, s.x + 1); }
        break;
    case 'K': if (args[0] <= 2) erase(s.y, args[0] == 0 ? s.x : 0, args[0] == 1 ? s.x + 1 : columns_); break;
    case 'X': erase(s.y, s.x, std::min(columns_, s.x + n)); break;
    case 'P': { auto& line = s.cells[s.y]; const int count = std::min(n, columns_ - s.x); std::move(line.begin() + s.x + count, line.end(), line.begin() + s.x); std::fill(line.end() - count, line.end(), blank()); repair(s.y); break; }
    case '@': { auto& line = s.cells[s.y]; const int count = std::min(n, columns_ - s.x); std::move_backward(line.begin() + s.x, line.end() - count, line.end()); std::fill(line.begin() + s.x, line.begin() + s.x + count, blank()); repair(s.y); break; }
    case 'S': scrollUp(n); break;
    case 'T': scrollDown(n); break;
    case 'L': case 'M': if (s.y >= top_ && s.y <= bottom_) { const int old = top_; top_ = s.y; if (command == 'L') scrollDown(n); else scrollUp(n); top_ = old; } break;
    case 'r': if (!priv && n < arg(1, rows_) && arg(1, rows_) <= rows_) { top_ = n - 1; bottom_ = arg(1, rows_) - 1; s.x = 0; s.y = origin_ ? top_ : 0; } break;
    case 's': s.savedX = s.x; s.savedY = s.y; break;
    case 'u': s.x = std::clamp(s.savedX, 0, columns_ - 1); s.y = std::clamp(s.savedY, 0, rows_ - 1); break;
    case 'm': if (!priv && !secondary) sgr(args); break;
    case 'n': if (!priv && reply) { if (args[0] == 5) reply("\x1b[0n"); else if (args[0] == 6) reply("\x1b[" + std::to_string(s.y + 1) + ";" + std::to_string(s.x + 1) + "R"); } break;
    case 'c': if (reply && !secondary) reply("\x1b[?1;2c"); break;
    case 't': if (!priv && args[0] == 18 && reply) reply("\x1b[8;" + std::to_string(rows_) + ";" + std::to_string(columns_) + "t"); break;
    case 'h': case 'l':
        if (priv) for (int mode : args) {
            const bool on = command == 'h';
            if (mode == 1) applicationCursor_ = on;
            else if (mode == 6) { origin_ = on; active().x = 0; active().y = on ? top_ : 0; }
            else if (mode == 7) autoWrap_ = on;
            else if (mode == 25) cursorVisible_ = on;
            else if (mode == 2004) bracketedPaste_ = on;
            else if (mode == 2026) synchronizedOutput_ = on;
            else if (mode == 1049 || mode == 47 || mode == 1047) {
                if (on != alternate_) {
                    alternate_ = on; top_ = 0; bottom_ = rows_ - 1;
                    if (on) { alternateScreen_ = {}; alternateScreen_.cells.assign(rows_, std::vector<Cell>(columns_)); }
                }
            }
        }
        break;
    default: break;
    }
}
void TerminalModel::osc() {
    if (!reply) return;
    // Theme queries only. Title, hyperlinks and clipboard commands have no side effects.
    if (sequence_ == "10;?") reply("\x1b]10;rgb:d3d3/dada/e3e3\x1b\\");
    else if (sequence_ == "11;?") reply("\x1b]11;rgb:1616/1919/1f1f\x1b\\");
}
void TerminalModel::feed(std::string_view bytes) {
    // Only an incomplete UTF-8 scalar persists between calls; control strings are bounded.
    std::string input;
    if (!utf8_.empty()) { input = std::move(utf8_); utf8_.clear(); input.append(bytes); bytes = input; }
    for (size_t p = 0; p < bytes.size();) {
        const auto c = static_cast<unsigned char>(bytes[p]);
        if (parse_ == Parse::Osc || parse_ == Parse::Discard) {
            ++p;
            if (c == 0x1b) parse_ = parse_ == Parse::Osc ? Parse::OscEscape : Parse::DiscardEscape;
            else if (c == 7 && parse_ == Parse::Osc) { osc(); parse_ = Parse::Text; }
            else if (parse_ == Parse::Osc && sequence_.size() < 256) sequence_ += static_cast<char>(c);
            else if (parse_ == Parse::Osc) sequence_ = "!overflow";
            continue;
        }
        if (parse_ == Parse::OscEscape || parse_ == Parse::DiscardEscape) {
            ++p;
            if (c == '\\') { if (parse_ == Parse::OscEscape) osc(); parse_ = Parse::Text; }
            else if (c != 0x1b) parse_ = parse_ == Parse::OscEscape ? Parse::Osc : Parse::Discard;
            continue;
        }
        if (c == 0x1b) { parse_ = Parse::Escape; ++p; continue; }
        if (parse_ == Parse::Charset) { ++p; parse_ = Parse::Text; continue; }
        if (parse_ == Parse::Escape) {
            ++p; parse_ = Parse::Text;
            auto& s = active();
            if (c == '[') { sequence_.clear(); parse_ = Parse::Csi; }
            else if (c == ']') { sequence_.clear(); parse_ = Parse::Osc; }
            else if (c == 'P' || c == '_' || c == '^' || c == 'X') parse_ = Parse::Discard;
            else if (c == '(' || c == ')' || c == '*' || c == '+') parse_ = Parse::Charset;
            else if (c == '7') { s.savedX = s.x; s.savedY = s.y; }
            else if (c == '8') { s.x = std::clamp(s.savedX, 0, columns_ - 1); s.y = std::clamp(s.savedY, 0, rows_ - 1); }
            else if (c == 'D') newline();
            else if (c == 'E') { s.x = 0; newline(); }
            else if (c == 'M') { if (s.y == top_) scrollDown(1); else s.y = std::max(0, s.y - 1); }
            else if (c == 'c') reset();
            continue;
        }
        if (parse_ == Parse::Csi) {
            ++p;
            if (c >= 0x40 && c <= 0x7e) { csi(static_cast<char>(c)); parse_ = Parse::Text; }
            else if (c >= 0x20 && c <= 0x3f) { if (sequence_.size() < 256) sequence_ += static_cast<char>(c); else parse_ = Parse::CsiDiscard; }
            else if (c == 0x18 || c == 0x1a) parse_ = Parse::Text;
            continue;
        }
        if (parse_ == Parse::CsiDiscard) { ++p; if (c >= 0x40 && c <= 0x7e) parse_ = Parse::Text; continue; }
        auto& s = active();
        if (c < 32 || c == 127) {
            ++p;
            if (c == '\r') { s.x = 0; s.wrap = false; }
            else if (c == '\n' || c == '\v' || c == '\f') newline();
            else if (c == '\b') { s.x = std::max(0, s.x - 1); s.wrap = false; }
            else if (c == '\t') { s.x = std::min(columns_ - 1, (s.x / 8 + 1) * 8); s.wrap = false; }
            continue;
        }
        const size_t start = p;
        const auto scalar = decodeUtf8(bytes, p, false);
        if (p == start) { utf8_.assign(bytes.substr(p)); break; }
        put(scalar);
    }
}
}
