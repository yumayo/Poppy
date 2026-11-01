#include "html_preview.h"
#include <iostream>
#include <stdexcept>

using namespace poppy;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
ResourceId local(std::string_view path) { return std::filesystem::u8path(path); }
}
int main() {
    try {
        for (auto extension : {L".html", L".htm", L".HTML", L".HtM"}) check(isHtmlExtension(extension), "HTML extension rejected");
        for (auto extension : {L"", L"html", L".html.txt", L".xhtml", L".svg", L".js"}) check(!isHtmlExtension(extension), "Non-HTML extension accepted");
        check(htmlFileUri(local("C:/site/index.html")) == "file:///C:/site/index.html", "Drive URI changed");
        check(htmlFileUri(local("C:\\my site\\index #1%.html")) == "file:///C:/my%20site/index%20%231%25.html", "Windows filename was not escaped");
        check(htmlFileUri(local(u8"C:/日本語/🍀.html")) == "file:///C:/%E6%97%A5%E6%9C%AC%E8%AA%9E/%F0%9F%8D%80.html", "Unicode URI changed");
        check(htmlFileUri(local("\\\\server\\share\\site & files\\index.html")) == "file://server/share/site%20%26%20files/index.html", "UNC URI changed");
        check(htmlFileUri(local("\\\\?\\C:\\site\\index.html")) == "file:///C:/site/index.html", "Extended drive URI changed");
        check(htmlFileUri(local("\\\\?\\UNC\\server\\share\\index.html")) == "file://server/share/index.html", "Extended UNC URI changed");
        const auto project = ResourceId::wsl("Ubuntu", "/home/user/project", "user");
        check(htmlFileUri(project / std::string_view("site/index #1?.html")) == "file://wsl.localhost/Ubuntu/home/user/project/site/index%20%231%3F.html", "WSL URI used the project root instead of the file");
        bool rejected = false;
        try { htmlFileUri(project / std::string_view("bad\\name.html")); } catch (const std::runtime_error&) { rejected = true; }
        check(rejected, "Unrepresentable WSL filename was accepted");
        rejected = false;
        try { htmlFileUri({}); } catch (const std::runtime_error&) { rejected = true; }
        check(rejected, "Empty filename was accepted");
        std::cout << "HTML preview tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
