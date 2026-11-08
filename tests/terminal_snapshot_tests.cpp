#include "terminal_model.h"
#include <iostream>
#include <stdexcept>

using poppy::TerminalModel;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void continuation() {
    const std::string stream = u8"before\r\n日本語😀\x1b[31;44mred\x1b[0m\x1b]2;working 日本語\x1b\\"
        "\x1b[2;4r\x1b[?6h\x1b[2;2Htext\x1b" "7\x1b[?1h\x1b[?2004h\x1b[?2026h"
        "\x1b[?1049halternate\x1b[?25l\x1b[6n\x1b[?2026$p\x1b[?1049l\x1b" "8\x1b[?2026l";
    for (size_t split = 0; split <= stream.size(); ++split) {
        TerminalModel original(12, 5); original.feed(std::string_view(stream).substr(0, split));
        const auto saved = original.snapshot(); auto restored = TerminalModel::fromSnapshot(saved);
        check(restored.snapshot() == saved, "Snapshot changed screens, history, modes or parser state");
        std::string firstReplies, secondReplies;
        original.reply = [&](std::string text) { firstReplies += text; };
        restored.reply = [&](std::string text) { secondReplies += text; };
        const auto remainder = std::string_view(stream).substr(split); original.feed(remainder); restored.feed(remainder);
        check(original.snapshot() == restored.snapshot() && firstReplies == secondReplies, "Restored parser did not continue identically at a byte boundary");
    }
}
void boundedHistory() {
    TerminalModel original(500, 200);
    for (int i = 0; i < 2200; ++i) original.feed("line " + std::to_string(i) + u8" 日本語\r\n");
    original.resize(23, 7);
    original.feed("abcdefghijklmnopqrstuvw01234567890123456789\r\n");
    original.feed("\x1b[?1049h\x1b[35mAI screen\x1b[?2004h");
    auto restored = TerminalModel::fromSnapshot(original.snapshot());
    check(original.snapshot() == restored.snapshot(), "Bounded history with old row widths or hidden normal screen was lost");
    original.feed("\x1b[?1049l"); restored.feed("\x1b[?1049l");
    check(restored.historySize() > 0 && original.snapshot() == restored.snapshot(), "Leaving alternate screen lost restored scrollback");
}
void malformed() {
    const auto saved = TerminalModel(4, 2).snapshot();
    const auto rejects = [](std::string_view bytes) {
        try { TerminalModel::fromSnapshot(bytes); } catch (const std::exception&) { return; }
        throw std::runtime_error("Malformed terminal snapshot was accepted");
    };
    for (size_t i = 0; i < saved.size(); ++i) rejects(std::string_view(saved).substr(0, i));
    rejects(saved + "trailing");
    auto version = saved; version[0] = 2; rejects(version);
    auto dimensions = saved; for (int i = 4; i < 8; ++i) dimensions[i] = '\xff'; rejects(dimensions);
}
}
int main() {
    try { continuation(); boundedHistory(); malformed(); std::cout << "Terminal snapshot tests passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
