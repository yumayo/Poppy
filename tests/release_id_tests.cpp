#include "release_id.h"
#include <iostream>
#include <stdexcept>

namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
}
int main() {
    try {
        const std::string id = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        for (const auto& suffix : {"", "\n", "\r\n"})
            check(poppy::parseReleaseId(id + suffix) == id, "A valid release ID was rejected");
        for (const auto& value : {std::string{}, std::string("../PoppyApp.dll"), id.substr(1), id + "0", id + "\n\n",
                " " + id, id + " ", id + "\r", "G" + id.substr(1), std::string("C:\\") + id,
                id.substr(0, 31) + "/" + id.substr(32), id.substr(0, 63) + std::string(1, '\0')}) {
            bool rejected = false;
            try { poppy::parseReleaseId(value); } catch (const std::invalid_argument&) { rejected = true; }
            check(rejected, "An invalid ID could escape the release directory or select an incomplete release");
        }
        std::cout << "Release ID tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
