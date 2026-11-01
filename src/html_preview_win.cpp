#include "html_preview.h"
#include "render.h"
#include <WebView2.h>
#include <shlobj.h>
#include <urlmon.h>
#include <atomic>
#include <cstring>

namespace poppy {
namespace {
// Use explicit interface IDs so the official SDK also builds with MinGW.
constexpr GUID environmentHandlerId{0x4e8a3389,0xc9d8,0x4bd2,{0xb6,0xb5,0x12,0x4f,0xee,0x6c,0xc1,0x4d}};
constexpr GUID controllerHandlerId{0x6c4819f3,0xc9b7,0x4260,{0x81,0x27,0xc9,0xf5,0xbd,0xe7,0xf6,0x8c}};
constexpr GUID focusHandlerId{0x05ea24bd,0x6452,0x4926,{0x90,0x14,0x4b,0x82,0xb4,0x98,0x13,0x5d}};
constexpr GUID navigationHandlerId{0xd33a35bf,0x1c49,0x4f98,{0x93,0xab,0x00,0x6e,0x05,0x33,0xfe,0x1c}};
constexpr GUID navigationStartingHandlerId{0x9adbe429,0xf36d,0x432b,{0x9d,0xdc,0xf8,0x88,0x1f,0xbd,0x76,0xe3}};
template<class Interface, class... Args> class Callback final : public Interface {
public:
    Callback(const GUID& id, std::function<void(Args...)> action) : id_(id), action_(std::move(action)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (!IsEqualIID(id, IID_IUnknown) && !IsEqualIID(id, id_)) return E_NOINTERFACE;
        *object = static_cast<Interface*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto count = --references_; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE Invoke(Args... args) override {
        // Never allow a C++ exception to cross a COM callback boundary.
        try { action_(args...); return S_OK; } catch (...) { return E_FAIL; }
    }
private:
    std::atomic<ULONG> references_{1};
    GUID id_;
    std::function<void(Args...)> action_;
};
template<class T> void retain(ComPtr<T>& target, T* object) { object->AddRef(); *target.put() = object; }
std::wstring executableDirectory() {
    std::wstring name(32768, L'\0');
    const auto count = GetModuleFileNameW(nullptr, name.data(), static_cast<DWORD>(name.size()));
    if (!count || count >= name.size()) throw std::runtime_error("Cannot locate Poppy.exe.");
    name.resize(count); return fs::path(name).parent_path().wstring();
}
HMODULE loader() {
    // Keep the loader mapped while any asynchronous WebView2 callbacks exist.
    static HMODULE module = nullptr;
    if (!module) module = LoadLibraryExW((executableDirectory() + L"\\WebView2Loader.dll").c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    return module;
}
bool sameUri(const std::wstring& expected, const wchar_t* actual) {
    if (!actual) return false;
    if (expected == actual) return true;
    // WebView2 can canonicalize file URIs, including UNC host casing.
    ComPtr<IUri> first, second;
    BOOL equal = FALSE;
    return SUCCEEDED(CreateUri(expected.c_str(), Uri_CREATE_CANONICALIZE, 0, first.put())) &&
        SUCCEEDED(CreateUri(actual, Uri_CREATE_CANONICALIZE, 0, second.put())) &&
        SUCCEEDED(first->IsEqual(second.get(), &equal)) && equal;
}
}
struct HtmlPreview::State : std::enable_shared_from_this<HtmlPreview::State> {
    HWND parent = nullptr;
    RECT bounds{};
    std::wstring uri, message = L"Loading HTML preview…";
    std::wstring pendingUri;
    UINT64 navigationId = 0;
    bool active = false, starting = false, failed = false;
    std::function<void()> changed, focused;
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    void status(std::wstring value) { message = std::move(value); if (active && changed) changed(); }
    void fail(HRESULT error) {
        starting = false; failed = true;
        if (controller) controller->put_IsVisible(FALSE);
        status(L"HTML preview could not start.\nInstall Microsoft Edge WebView2 Runtime and place WebView2Loader.dll beside Poppy.exe.\n\nHRESULT " +
            std::to_wstring(static_cast<unsigned long>(error)) + L"\n\nUse Source to return to the editor.");
    }
    void resized() {
        if (controller) { controller->put_Bounds(bounds); controller->NotifyParentWindowPositionChanged(); }
    }
    void navigate() {
        if (!active || uri.empty() || !webview) return;
        navigationId = 0; pendingUri = uri;
        controller->put_IsVisible(FALSE);
        webview->Stop();
        const auto error = webview->Navigate(uri.c_str());
        if (FAILED(error)) fail(error);
    }
    void hide() {
        if (!active) return;
        active = false; uri.clear(); pendingUri.clear(); navigationId = 0;
        if (controller) controller->put_IsVisible(FALSE);
        // Hiding alone leaves timers, scripts and audio running in the old page.
        if (webview) { webview->Stop(); webview->Navigate(L"about:blank"); }
    }
    void created(HRESULT error, ICoreWebView2Controller* result) {
        starting = false;
        if (FAILED(error) || !result) { fail(FAILED(error) ? error : E_FAIL); return; }
        retain(controller, result);
        controller->put_IsVisible(FALSE);
        error = controller->get_CoreWebView2(webview.put());
        if (FAILED(error) || !webview) { fail(FAILED(error) ? error : E_FAIL); return; }
        const auto weak = weak_from_this();
        auto* focus = new Callback<ICoreWebView2FocusChangedEventHandler, ICoreWebView2Controller*, IUnknown*>(focusHandlerId,
            [weak](ICoreWebView2Controller*, IUnknown*) { if (auto state = weak.lock()) if (state->active && state->focused) state->focused(); });
        EventRegistrationToken token{};
        error = controller->add_GotFocus(focus, &token); focus->Release();
        if (FAILED(error)) { fail(error); return; }
        auto* starting = new Callback<ICoreWebView2NavigationStartingEventHandler, ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*>(navigationStartingHandlerId,
            [weak](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) {
                const auto state = weak.lock(); if (!state || !state->active || state->uri.empty()) return;
                PWSTR uri = nullptr;
                if (FAILED(args->get_Uri(&uri))) return;
                const bool expected = state->pendingUri.empty() || sameUri(state->pendingUri, uri);
                CoTaskMemFree(uri);
                if (!expected) { args->put_Cancel(TRUE); return; }
                state->pendingUri.clear();
                args->get_NavigationId(&state->navigationId);
            });
        error = webview->add_NavigationStarting(starting, &token); starting->Release();
        if (FAILED(error)) { fail(error); return; }
        auto* navigation = new Callback<ICoreWebView2NavigationCompletedEventHandler, ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*>(navigationHandlerId,
            [weak](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) {
                const auto state = weak.lock(); if (!state || !state->active || state->uri.empty() || !state->navigationId) return;
                UINT64 id = 0; args->get_NavigationId(&id);
                if (id != state->navigationId) return;
                BOOL success = FALSE; args->get_IsSuccess(&success);
                COREWEBVIEW2_WEB_ERROR_STATUS error = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                args->get_WebErrorStatus(&error);
                if (!success && error == COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED) return;
                state->controller->put_IsVisible(success);
                state->status(success ? L"" : L"Cannot load this HTML file.\nCheck that it exists and is accessible from Windows.\nUse Reload to try again, or Source to return to the editor.");
                if (success && GetFocus() == state->parent) state->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            });
        error = webview->add_NavigationCompleted(navigation, &token); navigation->Release();
        if (FAILED(error)) { fail(error); return; }
        resized(); navigate();
    }
    void start() {
        starting = true;
        const auto module = loader();
        if (!module) { fail(HRESULT_FROM_WIN32(GetLastError())); return; }
        const auto procedure = GetProcAddress(module, "CreateCoreWebView2EnvironmentWithOptions");
        if (!procedure) { fail(HRESULT_FROM_WIN32(GetLastError())); return; }
        decltype(&CreateCoreWebView2EnvironmentWithOptions) create = nullptr;
        static_assert(sizeof(create) == sizeof(procedure));
        std::memcpy(&create, &procedure, sizeof(create));
        PWSTR local = nullptr;
        auto error = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local);
        if (FAILED(error)) { fail(error); return; }
        const auto profile = fs::path(local) / L"Poppy" / L"WebView2";
        CoTaskMemFree(local);
        const auto weak = weak_from_this();
        auto* handler = new Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT, ICoreWebView2Environment*>(environmentHandlerId,
            [weak](HRESULT error, ICoreWebView2Environment* environment) {
                const auto state = weak.lock(); if (!state) return;
                if (FAILED(error) || !environment) { state->fail(FAILED(error) ? error : E_FAIL); return; }
                retain(state->environment, environment);
                auto* handler = new Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, HRESULT, ICoreWebView2Controller*>(controllerHandlerId,
                    [weak](HRESULT error, ICoreWebView2Controller* controller) {
                        if (auto state = weak.lock()) state->created(error, controller);
                        else if (controller) controller->Close();
                    });
                error = environment->CreateCoreWebView2Controller(state->parent, handler); handler->Release();
                if (FAILED(error)) state->fail(error);
            });
        error = create(nullptr, profile.c_str(), nullptr, handler); handler->Release();
        if (FAILED(error)) fail(error);
    }
    ~State() { if (controller) controller->Close(); }
};
HtmlPreview::~HtmlPreview() { close(); }
void HtmlPreview::show(HWND parent, const ResourceId& path, const RECT& bounds) {
    hide();
    if (state_ && (state_->parent != parent || state_->failed)) close();
    if (!state_) state_ = std::make_shared<State>();
    state_->parent = parent; state_->bounds = bounds; state_->active = true;
    state_->changed = changed; state_->focused = focused;
    state_->status(L"Loading HTML preview…");
    try {
        state_->uri = wide(htmlFileUri(path));
        state_->resized();
        if (state_->webview) state_->navigate();
        else if (!state_->starting) state_->start();
    }
    catch (const std::exception& error) {
        if (!state_->uri.empty()) { state_->starting = false; state_->failed = true; }
        state_->status(wide(error.what()));
    }
}
void HtmlPreview::hide() { if (state_) state_->hide(); }
void HtmlPreview::close() {
    if (state_) { state_->active = false; state_->changed = {}; state_->focused = {}; }
    state_.reset();
}
void HtmlPreview::layout(const RECT& bounds) { if (state_) { state_->bounds = bounds; state_->resized(); } }
void HtmlPreview::focus() { if (active() && state_->controller && state_->message.empty()) state_->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC); }
bool HtmlPreview::active() const { return state_ && state_->active; }
std::wstring HtmlPreview::status() const { return active() ? state_->message : L""; }
}
