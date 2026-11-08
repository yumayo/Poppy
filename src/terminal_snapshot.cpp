#include "terminal_model.h"
#include "terminal_wire.h"
#include <limits>

namespace poppy {
std::string TerminalModel::snapshot() const {
    terminalWire::Writer out;
    out.number(terminalWire::version);
    out.number(columns_); out.number(rows_); out.number(top_); out.number(bottom_);
    out.number(static_cast<uint64_t>(firstRow_), 8); out.number(generation_, 8);
    out.number(fg_); out.number(bg_);
    for (bool value : {inverse_, bold_, alternate_, cursorVisible_, bracketedPaste_, applicationCursor_, autoWrap_, origin_, synchronizedOutput_, oscOverflow_}) out.flag(value);
    out.number(static_cast<unsigned>(parse_)); out.string(sequence_); out.string(utf8_); out.string(title_);
    const auto row = [&](const std::vector<Cell>& cells, int wrapped) {
        out.number(cells.size()); out.number(wrapped);
        for (const auto& cell : cells) { out.number(cell.ch); out.number(cell.fg); out.number(cell.bg); out.flag(cell.continuation); }
    };
    for (const Screen* screen : {&normal_, &alternateScreen_}) {
        out.number(screen->x); out.number(screen->y); out.number(screen->savedX); out.number(screen->savedY); out.flag(screen->wrap);
        for (int y = 0; y < rows_; ++y) row(screen->cells[y], screen->wrapped[y]);
    }
    out.number(history_.size());
    for (const auto& line : history_) row(line.cells, line.wrapped);
    return std::move(out.bytes);
}
TerminalModel TerminalModel::fromSnapshot(std::string_view bytes) {
    terminalWire::Reader in(bytes);
    if (in.number() != terminalWire::version) throw std::runtime_error("Incompatible terminal host; close terminals before updating this version");
    const int columns = static_cast<int>(in.bounded(2, 500)), rows = static_cast<int>(in.bounded(1, 200));
    TerminalModel model(columns, rows);
    model.top_ = static_cast<int>(in.bounded(0, rows - 1)); model.bottom_ = static_cast<int>(in.bounded(model.top_, rows - 1));
    const auto first = in.number(8); if (first > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) in.invalid();
    model.firstRow_ = static_cast<int64_t>(first); model.generation_ = in.number(8);
    model.fg_ = in.bounded(0, 0xffffff); model.bg_ = in.bounded(0, 0xffffff);
    for (bool* value : {&model.inverse_, &model.bold_, &model.alternate_, &model.cursorVisible_, &model.bracketedPaste_, &model.applicationCursor_, &model.autoWrap_, &model.origin_, &model.synchronizedOutput_, &model.oscOverflow_}) *value = in.flag();
    model.parse_ = static_cast<Parse>(in.bounded(0, static_cast<unsigned>(Parse::Charset)));
    model.sequence_ = in.string(256); model.utf8_ = in.string(3); model.title_ = in.string(1024);
    const auto row = [&](std::vector<Cell>& cells, int& wrapped, bool history) {
        const auto width = in.bounded(history ? 2 : columns, history ? 500 : columns);
        wrapped = static_cast<int>(in.bounded(0, width)); cells.resize(width);
        for (auto& cell : cells) {
            cell.ch = static_cast<char32_t>(in.bounded(0, 0x10ffff));
            if (cell.ch >= 0xd800 && cell.ch <= 0xdfff) in.invalid();
            cell.fg = in.bounded(0, 0xffffff); cell.bg = in.bounded(0, 0xffffff); cell.continuation = in.flag();
        }
    };
    for (Screen* screen : {&model.normal_, &model.alternateScreen_}) {
        screen->x = static_cast<int>(in.bounded(0, columns - 1)); screen->y = static_cast<int>(in.bounded(0, rows - 1));
        screen->savedX = static_cast<int>(in.bounded(0, 499)); screen->savedY = static_cast<int>(in.bounded(0, 199)); screen->wrap = in.flag();
        for (int y = 0; y < rows; ++y) row(screen->cells[y], screen->wrapped[y], false);
    }
    const auto count = in.bounded(0, 1000);
    for (uint32_t i = 0; i < count; ++i) {
        HistoryRow line; row(line.cells, line.wrapped, true); model.historyCells_ += line.cells.size();
        if (model.historyCells_ > 131072) in.invalid();
        model.history_.push_back(std::move(line));
    }
    in.finish(); return model;
}
}
