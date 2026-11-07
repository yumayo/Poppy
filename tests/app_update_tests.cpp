#include "app_update.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Fixture {
    fs::path root = fs::temp_directory_path() / ("poppy-update-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { fs::create_directories(root / "releases"); }
    ~Fixture() { std::error_code error; fs::remove_all(root, error); }
    fs::path release(char id) {
        const auto path = root / "releases" / std::string(64, id);
        fs::create_directories(path);
        std::ofstream(path / "PoppyApp.dll") << id;
        return path;
    }
    void select(const std::string& contents) { std::ofstream(root / "current.txt", std::ios::binary | std::ios::trunc) << contents; }
};
}
int main() {
    try {
        Fixture fixture;
        const auto running = fixture.release('a');
        check(!poppy::applicationUpdateAvailable(fixture.root, running), "Missing pointer offered a restart");
        fixture.select(std::string(64, 'a') + "\n");
        check(!poppy::applicationUpdateAvailable(fixture.root, running), "The running version offered a restart");
        const auto next = fixture.release('b');
        fixture.select(std::string(64, 'b') + "\r\n");
        check(poppy::applicationUpdateAvailable(fixture.root, running), "A newly published release was not detected");
        check(!poppy::applicationUpdateAvailable(fixture.root, next), "The restarted version still offered an update");
        check(!poppy::applicationUpdateAvailable(fixture.root, fixture.root), "An unversioned application offered an update");
        fs::create_directories(fixture.root / "other" / "releases" / std::string(64, 'a'));
        check(!poppy::applicationUpdateAvailable(fixture.root, fixture.root / "other" / "releases" / std::string(64, 'a')),
            "An application from another installation offered an update");
        for (const auto& invalid : {std::string{}, std::string("../PoppyApp.dll"), std::string(64, 'B'), std::string(65, 'b'),
                std::string(64, 'b') + "\n\n", std::string(64, 'b') + std::string(1, '\0')}) {
            fixture.select(invalid);
            check(!poppy::applicationUpdateAvailable(fixture.root, running), "An invalid pointer offered a restart");
        }
        fixture.select(std::string(64, 'c'));
        check(!poppy::applicationUpdateAvailable(fixture.root, running), "A missing release offered a restart");
        fs::create_directories(fixture.root / "releases" / std::string(64, 'c'));
        check(!poppy::applicationUpdateAvailable(fixture.root, running), "An incomplete release offered a restart");
        fixture.select(std::string(64, 'a'));
        check(poppy::applicationUpdateAvailable(fixture.root, next), "A published rollback was not detected");
        check(!poppy::applicationUpdateAvailable(fixture.root, running), "A withdrawn update was still offered");
        std::cout << "Published application update tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
