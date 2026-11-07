#include "project_runtime.h"
#include "shutdown_dialog.h"
#include "storage.h"
#include "terminal.h"
#include "project_provider.h"
#include "wsl_dialog.h"
#include "quick_open.h"
#include "single_instance.h"
#include "error_popup.h"
#include "resource_ids.h"
#include "launcher_api.h"
#include "scrollbars.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <algorithm>
#include <memory>
#include <map>
#include <optional>

namespace poppy {
namespace {
constexpr UINT wmStartup = WM_APP + 22;
constexpr UINT_PTR aiAnimationTimerId = 2;
constexpr int projectHeadingHeight = 48;
constexpr int projectMessageLineHeight = 18;
constexpr int projectMessageLines = 3;
constexpr int projectCardMargin = 8;
constexpr int projectCardGap = 8;
constexpr int projectBorderWidth = 5;
constexpr int projectCardBorderWidth = 2;
constexpr int projectCardRadius = 6;
constexpr int projectCardHeight = projectHeadingHeight + projectMessageLineHeight * projectMessageLines + 8;
constexpr int projectRowHeight = projectCardHeight + projectCardGap;
fs::path stateDirectory() {
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", nullptr, 0);
    if (length) { std::wstring path(length, 0); const auto count = GetEnvironmentVariableW(L"APPDATA", path.data(), length); if (!count || count >= length) throw std::runtime_error("Cannot read APPDATA"); path.resize(count); return fs::path(path) / L"yumayo" / L"Poppy"; }
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &value))) throw std::runtime_error("Cannot locate application data");
    fs::path path(value); CoTaskMemFree(value); return path / L"yumayo" / L"Poppy";
}
struct OpenProject {
    uint64_t id = 0;
    ResourceId path;
    std::unique_ptr<ProjectRuntime> runtime;
    bool missing = false, storageAvailable = true, dirty = false;
    ULONGLONG used = 0, changedAt = 0;
    uint64_t statRequest = 0;
};
struct ProjectDrag {
    uint64_t id;
    POINT origin, point;
    bool active = false;
    std::optional<size_t> position;
};
class Workspace {
public:
    int run(HINSTANCE instance, int show, const StartupOptions& options, SingleInstance& singleInstance);
private:
    static LRESULT CALLBACK procedure(HWND,UINT,WPARAM,LPARAM);
    static LRESULT CALLBACK scrollbarProcedure(HWND,UINT,WPARAM,LPARAM);
    LRESULT handle(UINT,WPARAM,LPARAM);
    void create();
    void showInitialWindow();
    void applyStartupRequests();
    void activateWindow();
    void layout();
    void paint();
    void choose();
    void chooseWindows();
    void chooseWsl();
    void showRecent();
    void remember(const ResourceId& path);
    void open(fs::path path, bool select = true);
    void open(ResourceId path, bool select = true);
    void select(size_t index, bool attention = false);
    void navigate(bool forward);
    void close(size_t index);
    void tick();
    void animateAiStatus();
    void save(OpenProject& project);
    void saveApp();
    void saveAll();
    void finishClose();
    void trimDocuments();
    void checkFolder(OpenProject& project);
    void markDirty();
    void updateTools();
    void updateProjectDrag(POINT point);
    void scrollProjectDrag();
    void endProjectDrag(bool drop = false);
    void scrollProjects(UINT command);
    int projectPage() const;
    int projectContentRight() const;
    RECT addButtonBounds() const;
    size_t hit(POINT point) const;
    RECT row(size_t index) const;
    OpenProject* find(uint64_t id);
    std::wstring subtitle(size_t index) const;
    void report(std::string error);
    HWND window_ = nullptr, tooltips_ = nullptr, projectScrollbar_ = nullptr;
    HINSTANCE instance_ = nullptr;
    SingleInstance* singleInstance_ = nullptr;
    // Present both indicator surfaces without serial waits for vertical sync.
    Renderer renderer_{D2D1_PRESENT_OPTIONS_IMMEDIATELY};
    Renderer scrollbarRenderer_{D2D1_PRESENT_OPTIONS_IMMEDIATELY};
    Scrollbars scrollbars_;
    QuickOpen recent_;
    ErrorPopup errors_;
    AiEventQueue events_{[this] { PostMessageW(window_, wmAiEvent, 0, 0); }};
    std::unique_ptr<Worker> storageWorker_;
    // This connection is created, used and destroyed on storageWorker_ only.
    std::unique_ptr<Storage> storage_;
    std::vector<OpenProject> projects_;
    std::vector<ResourceId> history_;
    NavigationHistory navigation_;
    std::optional<ProjectDrag> projectDrag_;
    AppSession session_;
    std::vector<fs::path> pendingProjects_;
    bool activationPending_ = false;
    size_t active_ = 0, tools_ = 0;
    uint64_t nextId_ = 1;
    UINT dpi_ = 96;
    UINT_PTR aiAnimationTimer_ = 0;
    int initialShow_ = SW_SHOWNORMAL;
    int scroll_ = 0, wheel_ = 0;
    bool listingWsl_ = false;
    bool initialShowPending_ = true;
    bool dragging_ = false, restoring_ = true, appDirty_ = false, closing_ = false;
    ULONGLONG appChanged_ = 0;
    std::wstring tipText_, storageError_;
    int px(int value) const { return MulDiv(value,dpi_,96); }
};
OpenProject* Workspace::find(uint64_t id) { const auto found=std::find_if(projects_.begin(),projects_.end(),[&](const OpenProject& project){return project.id==id;});return found==projects_.end()?nullptr:&*found; }
int Workspace::run(HINSTANCE instance,int show,const StartupOptions& options,SingleInstance& singleInstance) {
    initializeRendering();instance_=instance;initialShow_=show;singleInstance_=&singleInstance;
    for(const auto& project:options.projects)pendingProjects_.emplace_back(project);
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_WIN95_CLASSES};InitCommonControlsEx(&common);
    WNDCLASSEXW cls{};
    cls.cbSize=sizeof(cls);cls.lpfnWndProc=procedure;cls.hInstance=instance;cls.lpszClassName=L"PoppyMain";cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    cls.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(IDI_POPPY));
    cls.hIconSm=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(IDI_POPPY),IMAGE_ICON,GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),LR_SHARED));
    RegisterClassExW(&cls);
    window_=CreateWindowExW(0,cls.lpszClassName,L"Poppy",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,1380,820,nullptr,nullptr,instance,this);
    if(!window_)return 1;
    try { singleInstance.publish(window_); } catch (...) { DestroyWindow(window_); throw; }
    MSG message{};
    for (;;) {
        // Queued terminal output can continue immediately. Give input, paints
        // and timers a turn before taking another posted output notification.
        constexpr UINT uiMessages = PM_REMOVE | ((QS_INPUT | QS_PAINT | QS_TIMER) << 16);
        if (!PeekMessageW(&message, nullptr, 0, 0, uiMessages) && GetMessageW(&message, nullptr, 0, 0) <= 0) break;
        if (message.message == WM_QUIT) break;
        if ((message.hwnd == window_ || IsChild(window_, message.hwnd)) &&
            (message.message == WM_XBUTTONDOWN || message.message == WM_XBUTTONUP || message.message == WM_XBUTTONDBLCLK)) {
            if (message.message != WM_XBUTTONUP) navigate(GET_XBUTTON_WPARAM(message.wParam) == XBUTTON2);
            continue;
        }
        if(recent_.visible() && message.hwnd!=recent_.window() &&
            (message.message==WM_LBUTTONDOWN || message.message==WM_MBUTTONDOWN || message.message==WM_RBUTTONDOWN))recent_.close();
        // Application shortcuts must not intercept input destined for a terminal.
        const bool key=message.message==WM_KEYDOWN && !errors_.contains(message.hwnd) && (projects_.empty() || !projects_[active_].runtime->terminalInput(message.hwnd));
        if(key && message.wParam==VK_ESCAPE && projectDrag_){endProjectDrag();continue;}
        if(key && (GetKeyState(VK_CONTROL)&0x8000) && message.wParam=='O'){choose();continue;}
        if(key && (GetKeyState(VK_CONTROL)&0x8000) && !(GetKeyState(VK_SHIFT)&0x8000) && !(GetKeyState(VK_MENU)&0x8000) && message.wParam=='R'){showRecent();continue;}
        if(!projects_.empty() && (message.hwnd==window_ || IsChild(projects_[active_].runtime->window(),message.hwnd) || message.hwnd==projects_[active_].runtime->window())) {
            if((message.message>=WM_KEYFIRST&&message.message<=WM_KEYLAST)||(message.message>=WM_MOUSEFIRST&&message.message<=WM_MOUSELAST))markDirty();
            if (key && message.wParam == VK_F5) checkFolder(projects_[active_]);
            if(projects_[active_].runtime->key(message))continue;
        }
        TranslateMessage(&message);DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
void Workspace::create(){
    dpi_=GetDpiForWindow(window_);renderer_.setDpi(dpi_);scrollbarRenderer_.setDpi(dpi_);
    // Keep the themed client-area scrollbar separate from the main window's
    // native title bar and resize borders.
    WNDCLASSEXW scrollbarClass{};
    scrollbarClass.cbSize=sizeof(scrollbarClass);scrollbarClass.lpfnWndProc=scrollbarProcedure;
    scrollbarClass.hInstance=instance_;scrollbarClass.lpszClassName=L"PoppyProjectScrollbar";
    scrollbarClass.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassExW(&scrollbarClass);
    projectScrollbar_=CreateWindowExW(0,scrollbarClass.lpszClassName,nullptr,WS_CHILD|WS_VSCROLL,0,0,0,0,window_,nullptr,instance_,this);
    if(!projectScrollbar_)throw std::runtime_error("Cannot create project scrollbar");
    recent_.create(window_,instance_,QuickOpenMode::Projects);recent_.setDpi(dpi_);
    recent_.opened=[this](const ResourceId& path){open(path);};
    const BOOL dark=TRUE;DwmSetWindowAttribute(window_,20,&dark,sizeof(dark));
    tooltips_=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,window_,nullptr,instance_,nullptr);
    SendMessageW(tooltips_,TTM_SETMAXTIPWIDTH,0,700);
    storageWorker_=std::make_unique<Worker>(window_);
    storageWorker_->submit(L"load-app",[this]{
        AppSession state;std::vector<std::string> history;std::string error;
        try{storage_=std::make_unique<Storage>(stateDirectory());state=storage_->loadApp();history=storage_->projectHistory();}catch(const std::exception& failure){error=failure.what();storage_.reset();}
        storageWorker_->complete([this,state=std::move(state),history=std::move(history),error]{
            if(closing_)return;
            session_=state;restoring_=false;
            for(const auto& path:history)history_.push_back(ResourceId::parse(path));
            if(session_.positioned){
                RECT rect{session_.x,session_.y,session_.x+session_.width,session_.y+session_.height};MONITORINFO monitor{};monitor.cbSize=sizeof(monitor);
                if(GetMonitorInfoW(MonitorFromRect(&rect,MONITOR_DEFAULTTONEAREST),&monitor)){
                    const int width=std::min(rect.right-rect.left,monitor.rcWork.right-monitor.rcWork.left),height=std::min(rect.bottom-rect.top,monitor.rcWork.bottom-monitor.rcWork.top);
                    rect.left=std::clamp(rect.left,monitor.rcWork.left,monitor.rcWork.right-width);rect.top=std::clamp(rect.top,monitor.rcWork.top,monitor.rcWork.bottom-height);
                    SetWindowPos(window_,nullptr,rect.left,rect.top,width,height,SWP_NOZORDER|SWP_NOACTIVATE);
                }
            }
            const auto paths=session_.openProjects;
            if (!pendingProjects_.empty()) open(pendingProjects_.front(), false);
            else if (std::find(paths.begin(), paths.end(), session_.lastProject) != paths.end()) open(ResourceId::parse(session_.lastProject), false);
            for(const auto& path:paths)open(ResourceId::parse(path),false);
            std::map<std::wstring, size_t> order;
            for (size_t i = 0; i < paths.size(); ++i) order[pathKey(ResourceId::parse(paths[i]))] = i;
            std::stable_sort(projects_.begin(), projects_.end(), [&](const OpenProject& a, const OpenProject& b) {
                const auto left = order.find(pathKey(a.path)), right = order.find(pathKey(b.path));
                return (left == order.end() ? paths.size() : left->second) < (right == order.end() ? paths.size() : right->second);
            });
            if(!session_.lastProject.empty())for(size_t i=0;i<projects_.size();++i)if(pathKey(projects_[i].path)==pathKey(ResourceId::parse(session_.lastProject))){active_=i;break;}
            if(!pendingProjects_.empty())applyStartupRequests();
            else if(!projects_.empty())select(active_);
            layout();showInitialWindow();if(!error.empty())report(error);
        });
    });
    SetTimer(window_,1,80,nullptr);
}
void Workspace::showInitialWindow(){
    // Keep the window hidden until its placement and active project's layout
    // have been restored. File loading and WSL connections continue separately.
    if(!initialShowPending_||restoring_||(!projects_.empty()&&!projects_[active_].runtime->ready()))return;
    initialShowPending_=false;
    int show=activationPending_ ? SW_SHOWNORMAL : initialShow_;
    if(session_.positioned&&session_.maximized&&(show==SW_SHOWNORMAL||show==SW_SHOWDEFAULT||show==SW_SHOW))show=SW_SHOWMAXIMIZED;
    layout();ShowWindow(window_,show);UpdateWindow(window_);
    activateWindow();
    if(!storageError_.empty())report(utf8(storageError_));
}
void Workspace::applyStartupRequests(){
    if(restoring_||closing_)return;
    std::vector<fs::path> paths;paths.swap(pendingProjects_);
    for(const auto& path:paths)open(path);
    activateWindow();
}
void Workspace::activateWindow(){
    if(!activationPending_||initialShowPending_)return;
    activationPending_=false;
    if(IsIconic(window_))ShowWindow(window_,SW_RESTORE);
    SetForegroundWindow(window_);
}
void Workspace::choose(){
    endProjectDrag();
    recent_.close();
    HMENU menu = CreatePopupMenu(); AppendMenuW(menu, MF_STRING, 1, L"Open Windows folder…"); AppendMenuW(menu, MF_STRING | (listingWsl_ ? MF_GRAYED : 0), 2, L"Open WSL folder…");
    AppendMenuW(menu, MF_STRING, 3, L"Recent projects…\tCtrl+R");
    POINT point{}; GetCursorPos(&point); const auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window_, nullptr); DestroyMenu(menu);
    if (command == 1) chooseWindows(); else if (command == 2) chooseWsl(); else if (command == 3) showRecent();
}
void Workspace::showRecent(){
    if(restoring_)return;
    endProjectDrag();
    recent_.setProjects(history_);recent_.show();
}
void Workspace::remember(const ResourceId& path){
    const auto key=pathKey(path);
    const auto found=std::find_if(history_.begin(),history_.end(),[&](const ResourceId& entry){return pathKey(entry)==key;});
    if(found==history_.end())history_.insert(history_.begin(),path);
    else *found=path;
}
void Workspace::chooseWsl(){
    if (listingWsl_) return;
    listingWsl_ = true;
    storageWorker_->submit(L"list-wsl", [this] {
        std::vector<std::string> distributions; std::wstring error;
        try { distributions = installedWslDistributions(); } catch (const std::exception& failure) { error = wide(failure.what()); }
        storageWorker_->complete([this, distributions, error] {
            if(closing_)return;
            listingWsl_ = false;
            if (!error.empty()) { MessageBoxW(window_, error.c_str(), L"Poppy — WSL", MB_OK | MB_ICONWARNING); return; }
            if (const auto location = chooseWslFolder(window_, distributions)) open(*location);
        });
    });
}
void Workspace::chooseWindows(){
    IFileOpenDialog* dialog=nullptr;if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog))))return;
    DWORD options=0;dialog->GetOptions(&options);dialog->SetOptions(options|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST);dialog->SetTitle(L"Add project folder");
    if(SUCCEEDED(dialog->Show(window_))){IShellItem* item=nullptr;if(SUCCEEDED(dialog->GetResult(&item))){PWSTR path=nullptr;if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))){open(fs::path(path));CoTaskMemFree(path);}item->Release();}}
    dialog->Release();
}
void Workspace::open(fs::path path,bool shouldSelect){
    if(closing_)return;
    if (restoring_) { pendingProjects_.push_back(std::move(path)); return; }
    if (const auto location = wslUncLocation(path.u8string())) {
        storageWorker_->submit(L"normalize-wsl:" + path.native(), [this, location = *location, shouldSelect] {
            ResourceId resource; std::wstring error;
            try {
                const auto distributions = installedWslDistributions();
                const auto found = std::find_if(distributions.begin(), distributions.end(), [&](const std::string& distro) { return CompareStringOrdinal(wide(distro).c_str(), -1, wide(location.distro).c_str(), -1, TRUE) == CSTR_EQUAL; });
                if (found == distributions.end()) throw std::runtime_error("The WSL distribution is not registered: " + location.distro);
                resource = ResourceId::wsl(*found, location.root);
            } catch (const std::exception& failure) { error = wide(failure.what()); }
            storageWorker_->complete([this, resource, error, shouldSelect] { if(closing_)return; if (error.empty()) open(resource, shouldSelect); else MessageBoxW(window_, error.c_str(), L"Poppy — WSL", MB_OK | MB_ICONWARNING); });
        }); return;
    }
    open(ResourceId(path), shouldSelect);
}
void Workspace::open(ResourceId path,bool shouldSelect){
    if(closing_)return;
    if (restoring_) return;
    if (!path.wsl() && wslUncLocation(path.u8string())) { open(path.local(), shouldSelect); return; }
    if (!path.wsl()) { std::error_code error; const auto local = fs::absolute(path.local(), error).lexically_normal().make_preferred(); if (error) return; path = ResourceId(local); }
    recent_.close(false);
    const auto key=pathKey(path);
    for(size_t i=0;i<projects_.size();++i)if(pathKey(projects_[i].path)==key){if(shouldSelect)select(i);return;}
    if (projects_.size() >= 1000) return;
    remember(path);
    endProjectDrag();
    OpenProject project;project.id=nextId_++;project.path=path;project.runtime=std::make_unique<ProjectRuntime>();
    project.runtime->openFolder=[this](const fs::path& folder){open(folder);};
    project.runtime->chooseProject=[this]{choose();};
    project.runtime->changed=[this, id = project.id]{ if (auto project = find(id)) { project->dirty = true; project->changedAt = GetTickCount64(); } InvalidateRect(window_,nullptr,FALSE); };
    project.runtime->activated = [this, id = project.id](NavigationTarget target) {
        if (closing_ || projects_.empty() || projects_[active_].id != id) return;
        target.project = id; navigation_.visit(std::move(target));
    };
    project.runtime->error = [this, id = project.id](const std::string& key, const std::wstring& title, const std::wstring& message) {
        if (!closing_) errors_.show(window_, std::to_string(id) + ":" + key, title, message, dpi_);
    };
    project.runtime->resolved=[this, id = project.id](const ResourceId& resolved){
        if (auto project = find(id)) {
            for (const auto& existing : projects_) if (existing.id != id && pathKey(existing.path) == pathKey(resolved)) {
                if (!project->runtime->hasUnsaved()) PostMessageW(window_, WM_APP + 21, static_cast<WPARAM>(id), static_cast<LPARAM>(existing.id));
                return;
            }
            project->path = resolved; project->dirty = true; project->changedAt = GetTickCount64(); appDirty_ = true; appChanged_ = GetTickCount64();
            remember(resolved);
        }
    };
    project.runtime->create(window_,instance_,&events_);project.runtime->setDpi(dpi_);
    const auto id=project.id;projects_.push_back(std::move(project));
    if(shouldSelect)select(projects_.size()-1);else layout();
    storageWorker_->submit(L"load:"+std::to_wstring(id),[this,id,path,key]{
        std::optional<ProjectSession> state;std::string failure;std::error_code folderError;
        const bool missing=!path.wsl() && !fs::is_directory(path.local(),folderError);
        try{if(storage_)state=storage_->loadProject(utf8(key),path.serialized());}catch(const std::exception& error){failure=error.what();}
        if (state && !missing && !path.wsl()) {
            std::vector<SavedTab> tabs; size_t active = 0;
            for (size_t i = 0; i < state->tabs.size(); ++i) {
                const auto& tab = state->tabs[i]; std::error_code error;
                if (!tab.diff && !fs::is_regular_file(fs::u8path(tab.path), error)) continue;
                if (i <= state->activeTab) active = tabs.size();
                tabs.push_back(tab);
            }
            state->tabs = std::move(tabs); state->activeTab = active;
        }
        storageWorker_->complete([this,id,path,state=std::move(state),missing,failure]{
            if(closing_)return;
            auto project=find(id);if(!project)return;
            project->missing=missing;project->storageAvailable=failure.empty();
            project->runtime->initialize(path,state.value_or(ProjectSession{}),state.has_value());
            project->dirty=true;project->changedAt=GetTickCount64();
            if(project==&projects_[active_]){layout();project->runtime->activate(true);}
            updateTools();InvalidateRect(window_,nullptr,FALSE);showInitialWindow();if(!failure.empty())report(failure);
        });
    });
    appDirty_=true;appChanged_=GetTickCount64();
}
void Workspace::select(size_t index,bool attention){
    if(index>=projects_.size())return;
    endProjectDrag();
    if(active_<projects_.size()&&active_!=index){auto& previous=projects_[active_];previous.runtime->activate(false);previous.dirty=true;previous.changedAt=GetTickCount64();}
    active_=index;projects_[index].used=GetTickCount64();
    for(size_t i=0;i<projects_.size();++i)if(i!=active_)ShowWindow(projects_[i].runtime->window(),SW_HIDE);
    const int count=std::max(1,static_cast<int>(projects_.size()));
    const auto rect=row(index);RECT client{};GetClientRect(window_,&client);
    if(rect.top<px(44))scroll_=std::min<int>(static_cast<int>(index),count-1);
    else if(rect.bottom>client.bottom)scroll_=std::max(0,static_cast<int>(index)-projectPage()+1);
    layout();projects_[active_].runtime->activate(true);if(attention)projects_[active_].runtime->focusAttention();
    appDirty_=true;appChanged_=GetTickCount64();updateTools();InvalidateRect(window_,nullptr,FALSE);
    checkFolder(projects_[active_]);
    trimDocuments();
}
void Workspace::trimDocuments() {
    // Release only inactive document bodies. Trees, indexes and sessions remain.
    size_t total = 0;
    for (const auto& project : projects_) total += project.runtime->documentBytes();
    while (total > 64 * 1024 * 1024) {
        OpenProject* oldest = nullptr;
        for (size_t i = 0; i < projects_.size(); ++i) if (i != active_ && projects_[i].runtime->documentBytes() && (!oldest || projects_[i].used < oldest->used)) oldest = &projects_[i];
        if (!oldest) break;
        total -= oldest->runtime->documentBytes(); oldest->runtime->releaseDocument();
    }
}
void Workspace::navigate(bool forward) {
    if (closing_) return;
    navigation_.move(forward, [this](const NavigationTarget& target) {
        auto project = find(target.project);
        if (!project || !project->runtime->canNavigate(target)) return false;
        recent_.close(false); endProjectDrag();
        select(static_cast<size_t>(project - projects_.data()));
        project->runtime->navigate(target); markDirty(); return true;
    });
}
void Workspace::save(OpenProject& project){
    if(!project.runtime->ready()||!project.storageAvailable)return;
    const auto state=project.runtime->snapshot();const auto path=project.path;const auto key=utf8(pathKey(path));
    storageWorker_->submit(L"save:"+std::to_wstring(project.id),[this,state,path,key]{
        try{if(storage_)storage_->saveProject(key,path.serialized(),state);}catch(const std::exception& error){const std::string text=error.what();storageWorker_->complete([this,text]{report(text);});}
    });project.dirty=false;
}
void Workspace::saveApp(){
    if(restoring_||initialShowPending_)return;
    session_.openProjects.clear(); session_.openProjectKeys.clear();
    for(const auto& project:projects_) { session_.openProjects.push_back(project.path.serialized()); session_.openProjectKeys.push_back(utf8(pathKey(project.path))); }
    session_.lastProject=projects_.empty()?std::string{}:projects_[active_].path.serialized();
    WINDOWPLACEMENT placement{};placement.length=sizeof(placement);
    if(GetWindowPlacement(window_,&placement)){
        auto rect=placement.rcNormalPosition;MONITORINFO monitor{};monitor.cbSize=sizeof(monitor);
        if(GetMonitorInfoW(MonitorFromWindow(window_,MONITOR_DEFAULTTONEAREST),&monitor))OffsetRect(&rect,monitor.rcWork.left-monitor.rcMonitor.left,monitor.rcWork.top-monitor.rcMonitor.top);
        session_.x=rect.left;session_.y=rect.top;session_.width=rect.right-rect.left;session_.height=rect.bottom-rect.top;session_.positioned=true;
        session_.maximized=placement.showCmd==SW_SHOWMAXIMIZED||(placement.flags&WPF_RESTORETOMAXIMIZED)!=0;
    }
    const auto state=session_;storageWorker_->submit(L"save-app",[this,state]{try{if(storage_)storage_->saveApp(state);}catch(const std::exception& error){const std::string text=error.what();storageWorker_->complete([this,text]{report(text);});}});appDirty_=false;
}
void Workspace::saveAll(){for(auto& project:projects_)save(project);saveApp();storageWorker_->drain();storageWorker_->dispatch();}
void Workspace::finishClose(){
    closing_=true;
    endProjectDrag();recent_.close(false);
    if(GetCapture())ReleaseCapture();
    KillTimer(window_,1);KillTimer(window_,77);KillTimer(window_,aiAnimationTimerId);
    SendMessageW(tooltips_,TTM_ACTIVATE,FALSE,0);
    // Snapshot focus and session state before the modal window takes focus.
    for(auto& project:projects_)save(project);
    saveApp();
    std::unique_ptr<ShutdownDialog> progress;
    // Resource exhaustion must not prevent closing the application.
    try{progress=std::make_unique<ShutdownDialog>(window_,instance_);}catch(const std::exception&){}
    if(progress)progress->detail(L"Saving project state and open tabs…");
    if(progress)progress->wait([this]{storageWorker_->drain();});else storageWorker_->drain();
    storageWorker_->dispatch();
    for(size_t i=0;i<projects_.size();++i){
        const auto& path=projects_[i].path;
        const auto name=path.filename().empty()?path.native():path.filename().native();
        if(progress)progress->stage(ShutdownDialog::Stage::Stopping,L"Project "+std::to_wstring(i+1)+L" / "+std::to_wstring(projects_.size())+L" · "+name);
        projects_[i].runtime->shutdown(progress.get());
    }
    if(progress){progress->stage(ShutdownDialog::Stage::Finishing);progress->detail(L"Closing WSL connections and releasing project resources…");}
    for(auto& project:projects_)project.runtime->disconnect(progress.get());
    projects_.clear();
    if(progress)progress->detail(L"Closing workspace storage…");
    storageWorker_->submit(L"release-storage",[this]{storage_.reset();});
    if(progress)progress->wait([this]{storageWorker_->drain();storageWorker_->stop();});
    else{storageWorker_->drain();storageWorker_->stop();}
    storageWorker_.reset();
    if(!storageError_.empty())MessageBoxW(progress?progress->window():window_,(L"Some state could not be saved.\n\n"+storageError_).c_str(),L"Poppy",MB_OK|MB_ICONWARNING);
    progress.reset();
    DestroyWindow(window_);
}
void Workspace::close(size_t index){
    endProjectDrag();
    if(index>=projects_.size() || !projects_[index].runtime->canClose())return;
    save(projects_[index]);projects_.erase(projects_.begin()+static_cast<std::ptrdiff_t>(index));
    if(index<active_)--active_;
    if(!projects_.empty())select(std::min(active_,projects_.size()-1));else{active_=0;layout();}
    appDirty_=true;appChanged_=GetTickCount64();updateTools();InvalidateRect(window_,nullptr,FALSE);
}
void Workspace::checkFolder(OpenProject& project) {
    if (!project.runtime->ready() || project.path.wsl()) return;
    const auto path = project.path; const auto id = project.id; const auto request = ++project.statRequest;
    storageWorker_->submit(L"stat:" + std::to_wstring(id), [this, path, id, request] {
        std::error_code error; const bool missing = !fs::is_directory(path.local(), error);
        storageWorker_->complete([this, id, request, missing] {
            const auto project = find(id); if (closing_ || !project || project->statRequest != request) return;
            const bool restored = project->missing && !missing; project->missing = missing;
            if (restored) project->runtime->revalidate();
            InvalidateRect(window_, nullptr, FALSE);
        });
    });
}
void Workspace::markDirty(){if(!projects_.empty()){projects_[active_].dirty=true;projects_[active_].changedAt=GetTickCount64();}}
void Workspace::tick(){
    scrollProjectDrag();
    const auto now=GetTickCount64();
    for(size_t i=0;i<projects_.size();++i){auto& project=projects_[i];project.runtime->poll();if(project.dirty&&now-project.changedAt>=650)save(project);
    }
    trimDocuments();
    if(appDirty_&&now-appChanged_>=650)saveApp();
    animateAiStatus();
}
void Workspace::animateAiStatus() {
    bool running = false;
    if (!IsIconic(window_) && IsWindowVisible(window_)) {
        RECT client{}; GetClientRect(window_, &client);
        for (size_t i = 0; i < projects_.size(); ++i) {
            const auto item = row(i);
            if (item.top < px(44) || item.top >= client.bottom || projects_[i].runtime->summary().state != AiState::Running) continue;
            const RECT icon{item.left + px(4), item.top, item.left + px(30), item.top + px(projectHeadingHeight)};
            InvalidateRect(window_, &icon, FALSE); running = true;
        }
        if (!projects_.empty()) running = projects_[active_].runtime->animateAiStatus() || running;
    }
    // A 16 ms WM_TIMER can round up to two default Windows clock ticks.
    // Request the minimum interval for roughly one update per display refresh.
    if (running && !aiAnimationTimer_) aiAnimationTimer_ = SetTimer(window_, aiAnimationTimerId, USER_TIMER_MINIMUM, nullptr);
    else if (!running && aiAnimationTimer_) { KillTimer(window_, aiAnimationTimer_); aiAnimationTimer_ = 0; }
}
RECT Workspace::addButtonBounds() const {return {px(session_.projectListWidth)-px(34),0,px(session_.projectListWidth),px(44)};}
int Workspace::projectPage() const {
    RECT client{};GetClientRect(window_,&client);
    return std::max(1,(static_cast<int>(client.bottom)-px(44))/px(projectRowHeight));
}
int Workspace::projectContentRight() const {
    return px(session_.projectListWidth)-((GetWindowLongPtrW(projectScrollbar_,GWL_STYLE)&WS_VSCROLL)?GetSystemMetricsForDpi(SM_CXVSCROLL,dpi_):0);
}
void Workspace::scrollProjects(UINT command) {
    SCROLLINFO info{sizeof(info),SIF_ALL,0,0,0,0,0};GetScrollInfo(projectScrollbar_,SB_VERT,&info);
    switch(command){
    case SB_LINEUP:--scroll_;break;case SB_LINEDOWN:++scroll_;break;
    case SB_PAGEUP:scroll_-=projectPage();break;case SB_PAGEDOWN:scroll_+=projectPage();break;
    case SB_THUMBTRACK:case SB_THUMBPOSITION:scroll_=scrollbars_.trackPosition(SB_VERT,info.nTrackPos);break;
    case SB_TOP:scroll_=0;break;case SB_BOTTOM:scroll_=static_cast<int>(projects_.size());break;
    default:return;
    }
    layout();
}
RECT Workspace::row(size_t index) const {
    const int top=px(44)+(static_cast<int>(index)-scroll_)*px(projectRowHeight)+px(projectCardGap/2);
    return {px(projectCardMargin),top,projectContentRight()-px(projectCardMargin),top+px(projectCardHeight)};
}
size_t Workspace::hit(POINT point) const {
    RECT client{};GetClientRect(window_,&client);
    if(point.x<0||point.x>=projectContentRight()||point.y<px(44)||point.y>=client.bottom)return projects_.size();
    const auto index=static_cast<size_t>(scroll_+(point.y-px(44))/px(projectRowHeight));
    if(index<projects_.size()){const auto card=row(index);if(PtInRect(&card,point))return index;}
    return projects_.size();
}
void Workspace::updateProjectDrag(POINT point) {
    if (!projectDrag_) return;
    auto& drag = *projectDrag_; drag.point = point;
    if (!drag.active) {
        if (std::abs(point.x-drag.origin.x) < GetSystemMetricsForDpi(SM_CXDRAG,dpi_) &&
            std::abs(point.y-drag.origin.y) < GetSystemMetricsForDpi(SM_CYDRAG,dpi_)) return;
        drag.active = true;
        SendMessageW(tooltips_,TTM_ACTIVATE,FALSE,0);
    }
    drag.position.reset();
    RECT client{}; GetClientRect(window_,&client);
    if (point.x >= 0 && point.x < projectContentRight() && point.y >= px(44) && point.y < client.bottom) {
        const auto position = static_cast<size_t>(scroll_+(point.y-px(44)+px(projectRowHeight)/2)/px(projectRowHeight));
        drag.position = std::min(position,projects_.size());
    }
    InvalidateRect(window_,nullptr,FALSE);
}
void Workspace::scrollProjectDrag() {
    if (!projectDrag_ || !projectDrag_->active) return;
    const auto point = projectDrag_->point;
    RECT client{}; GetClientRect(window_,&client);
    if (point.x < 0 || point.x >= projectContentRight() || point.y < px(44) || point.y >= client.bottom) return;
    const int page = projectPage();
    int next = scroll_;
    if (point.y < px(44+24)) --next;
    else if (point.y >= client.bottom-px(24)) ++next;
    next = std::clamp(next,0,std::max(0,static_cast<int>(projects_.size())-page));
    if (next == scroll_) return;
    scroll_ = next; layout(); updateProjectDrag(point);
}
void Workspace::endProjectDrag(bool drop) {
    if (!projectDrag_) return;
    const auto drag = *projectDrag_; projectDrag_.reset();
    if (GetCapture() == window_) ReleaseCapture();
    if (drag.active) SendMessageW(tooltips_,TTM_ACTIVATE,TRUE,0);
    if (drop && drag.active && drag.position && *drag.position <= projects_.size()) {
        const auto source = find(drag.id);
        if (source) {
            const auto from = static_cast<size_t>(source-projects_.data());
            auto to = *drag.position; if (to > from) --to;
            if (to != from) {
                const auto selected = projects_[active_].id;
                auto begin = projects_.begin();
                if (to < from) std::rotate(begin+to,begin+from,begin+from+1);
                else std::rotate(begin+from,begin+from+1,begin+to+1);
                active_ = static_cast<size_t>(find(selected)-projects_.data());
                appDirty_ = true; appChanged_ = GetTickCount64();
                layout();
            }
        }
    }
    InvalidateRect(window_,nullptr,FALSE);
}
std::wstring Workspace::subtitle(size_t index) const {
    const auto& path=projects_[index].path;
    if (path.wsl()) { const auto state = projects_[index].runtime->connectionDescription(); return L"WSL · " + wide(path.location().distro) + (state.empty() || state == L"Connected" ? L"" : L" · " + state); }
    const auto name=path.filename().native();bool duplicate=false;
    for(size_t i=0;i<projects_.size();++i)if(i!=index&&CompareStringOrdinal(name.c_str(),-1,projects_[i].path.filename().native().c_str(),-1,TRUE)==CSTR_EQUAL)duplicate=true;
    if(!duplicate)return projects_[index].missing?L"Folder not found":L"";
    size_t depth=1;auto suffix=[](fs::path parent,size_t count){fs::path result;while(count--&&!parent.empty()){result=parent.filename()/result;const auto next=parent.parent_path();if(next==parent){result=parent.root_path()/result;break;}parent=next;}return result.native();};
    for(;;++depth){auto text=suffix(path.local().parent_path(),depth);bool unique=true;for(size_t i=0;i<projects_.size();++i)if(i!=index&&path.filename()==projects_[i].path.filename()&&text==suffix(projects_[i].path.wsl() ? fs::path{} : projects_[i].path.local().parent_path(),depth))unique=false;if(unique||depth>32)return text;}
}
void Workspace::layout(){
    if(!window_)return;
    RECT r{};GetClientRect(window_,&r);const int left=px(session_.projectListWidth)+px(projectBorderWidth);
    const int page=projectPage();scroll_=std::clamp(scroll_,0,std::max(0,static_cast<int>(projects_.size())-page));
    SCROLLINFO info{sizeof(info),SIF_RANGE|SIF_PAGE|SIF_POS,0,std::max(0,static_cast<int>(projects_.size())-1),static_cast<UINT>(page),scroll_,0};
    SetScrollInfo(projectScrollbar_,SB_VERT,&info,FALSE);
    const int scrollbarWidth=GetSystemMetricsForDpi(SM_CXVSCROLL,dpi_);
    SetWindowPos(projectScrollbar_,nullptr,px(session_.projectListWidth)-scrollbarWidth,px(44),scrollbarWidth,std::max(0L,r.bottom-px(44)),
        SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW|((static_cast<int>(projects_.size())>page)?SWP_SHOWWINDOW:SWP_HIDEWINDOW));
    // Do not copy old Direct2D pixels while moving the project and its panes.
    // Repaint the complete layout after the new positions have been applied.
    if(!projects_.empty())SetWindowPos(projects_[active_].runtime->window(),nullptr,left,0,std::max(0L,r.right-left),r.bottom,SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW|SWP_NOCOPYBITS);
    recent_.layout();
    errors_.layout();
    updateTools();
    // A continuous stream of mouse moves can delay queued WM_PAINT messages.
    RedrawWindow(window_,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_UPDATENOW);
}
void Workspace::updateTools(){
    for(size_t i=0;i<=tools_;++i){TOOLINFOW tip{};tip.cbSize=sizeof(tip);tip.hwnd=window_;tip.uId=i;SendMessageW(tooltips_,TTM_DELTOOLW,0,reinterpret_cast<LPARAM>(&tip));}
    tools_=projects_.size();RECT client{};GetClientRect(window_,&client);
    TOOLINFOW addTip{};addTip.cbSize=sizeof(addTip);addTip.hwnd=window_;addTip.uId=0;addTip.uFlags=TTF_SUBCLASS;addTip.rect=addButtonBounds();addTip.lpszText=const_cast<wchar_t*>(L"Open folder (Ctrl+O)");SendMessageW(tooltips_,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&addTip));
    for(size_t i=0;i<projects_.size();++i){auto rect=row(i);rect.top=std::max<LONG>(px(44),rect.top);rect.bottom=std::min<LONG>(client.bottom,rect.bottom);if(rect.top>=rect.bottom)continue;
        TOOLINFOW tip{};tip.cbSize=sizeof(tip);tip.hwnd=window_;tip.uId=i+1;tip.uFlags=TTF_SUBCLASS;tip.rect=rect;tip.lpszText=LPSTR_TEXTCALLBACKW;SendMessageW(tooltips_,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));}
}
void Workspace::paint(){
    Renderer::Paint paint(renderer_,window_);if(!paint)return;RECT client{};GetClientRect(window_,&client);renderer_.fill(client,background);
    const int width=px(session_.projectListWidth),borderWidth=px(projectBorderWidth);renderer_.fill({width,0,width+borderWidth,client.bottom},border);
    const int cardBorderWidth=px(projectCardBorderWidth),cardRadius=px(projectCardRadius);
    const auto addButton=addButtonBounds();
    renderer_.label({px(12),0,addButton.left,px(44)},L"PROJECTS",foreground);
    renderer_.label(addButton,L"+",accent,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    for(size_t i=0;i<projects_.size();++i){auto rect=row(i);if(rect.bottom<=px(44)||rect.top>=client.bottom)continue;
        const auto& project=projects_[i];const auto summary=project.runtime->summary();
        renderer_.fillRounded(rect,i==active_?accent:border,cardRadius);
        auto inside=rect;InflateRect(&inside,-cardBorderWidth,-cardBorderWidth);renderer_.fillRounded(inside,panel,cardRadius-cardBorderWidth);
        const int headingBottom=rect.top+px(projectHeadingHeight);
        drawAiStatus(renderer_,{rect.left+px(4),rect.top,rect.left+px(30),headingBottom},summary.state,dpi_);
        const auto secondary=subtitle(i);const auto title=project.path.filename().empty()?project.path.native():project.path.filename().native();
        renderer_.label({rect.left+px(32),rect.top,rect.right-px(22),rect.top+(secondary.empty()?px(projectHeadingHeight):px(27))},title,project.missing?muted:foreground);
        if(!secondary.empty())renderer_.label({rect.left+px(32),rect.top+px(24),rect.right-px(8),headingBottom},secondary,muted);
        renderer_.label({rect.left+px(32),headingBottom,rect.right-px(8),headingBottom+px(projectMessageLineHeight)*projectMessageLines},
            wide(summary.activity),muted,DT_LEFT|DT_TOP|DT_WORDBREAK|DT_END_ELLIPSIS,false,px(projectMessageLineHeight));
        if(summary.unread)renderer_.fill({rect.right-px(12),rect.top+px(10),rect.right-px(7),rect.top+px(15)},accent);
        const auto running=summary.counts[static_cast<size_t>(AiState::Running)];if(running&&summary.state!=AiState::Running)renderer_.label({rect.right-px(24),rect.top+px(22),rect.right-px(3),headingBottom},std::to_wstring(running),accent);
    }
    if(projectDrag_&&projectDrag_->active&&projectDrag_->position){
        const int y=std::clamp(static_cast<int>(row(*projectDrag_->position).top)-px(projectCardGap/2)-px(1),px(44),static_cast<int>(client.bottom)-px(2));
        renderer_.fill({px(projectCardMargin),y,projectContentRight()-px(projectCardMargin),y+px(2)},accent);
    }
    if(projects_.empty())renderer_.label({width+px(30),px(40),client.right-px(30),client.bottom},restoring_?L"Restoring projects…":L"Open a folder with Ctrl+O.",muted,DT_LEFT|DT_TOP|DT_WORDBREAK);
}
void Workspace::report(std::string error){storageError_=wide(error);if(!closing_&&!initialShowPending_)MessageBoxW(window_,(L"Could not read or save state.\n\n"+storageError_).c_str(),L"Poppy",MB_OK|MB_ICONWARNING);}
LRESULT CALLBACK Workspace::procedure(HWND w,UINT m,WPARAM a,LPARAM b){auto self=reinterpret_cast<Workspace*>(GetWindowLongPtrW(w,GWLP_USERDATA));if(m==WM_NCCREATE){self=static_cast<Workspace*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams);self->window_=w;SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}return self?self->handle(m,a,b):DefWindowProcW(w,m,a,b);}
LRESULT CALLBACK Workspace::scrollbarProcedure(HWND window,UINT message,WPARAM a,LPARAM b){
    auto self=reinterpret_cast<Workspace*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE){self=static_cast<Workspace*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
    if(self){
        LRESULT result=0;if(self->scrollbars_.handle(window,message,a,b,result))return result;
        switch(message){
        case WM_PAINT:{Renderer::Paint paint(self->scrollbarRenderer_,window);if(paint)self->scrollbars_.paint(window,self->scrollbarRenderer_,background);return 0;}
        case WM_ERASEBKGND:return 1;
        case WM_VSCROLL:self->scrollProjects(LOWORD(a));return 0;
        case WM_MOUSEWHEEL:return SendMessageW(self->window_,message,a,b);
        case WM_CONTEXTMENU:case WM_MOUSEHWHEEL:return 0;
        }
    }
    return DefWindowProcW(window,message,a,b);
}
LRESULT Workspace::handle(UINT m,WPARAM a,LPARAM b){switch(m){
case WM_CREATE:create();return 0;case WM_PAINT:paint();return 0;case WM_ERASEBKGND:return 1;case WM_SIZE:endProjectDrag();layout();if(!restoring_){appDirty_=true;appChanged_=GetTickCount64();}return 0;
case WM_GETMINMAXINFO:reinterpret_cast<MINMAXINFO*>(b)->ptMinTrackSize={px(900),px(480)};return 0;
case WM_DPICHANGED:{endProjectDrag();dpi_=HIWORD(a);renderer_.setDpi(dpi_);scrollbarRenderer_.setDpi(dpi_);recent_.setDpi(dpi_);errors_.setDpi(dpi_);for(auto& project:projects_)project.runtime->setDpi(dpi_);const auto rect=reinterpret_cast<RECT*>(b);SetWindowPos(window_,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER|SWP_NOACTIVATE);layout();return 0;}
case WM_MOVE: errors_.layout(); return 0;
case wmWork:if(!closing_&&storageWorker_)storageWorker_->dispatch();return 0;
case wmStartup:applyStartupRequests();return 0;
case WM_APP + 21:{ const auto project = find(static_cast<uint64_t>(a)); if (!project || project->runtime->hasUnsaved()) return 0; const bool active = project == &projects_[active_]; const auto index = static_cast<size_t>(project - projects_.data()); close(index); if (active) for (size_t i = 0; i < projects_.size(); ++i) if (projects_[i].id == static_cast<uint64_t>(b)) { select(i); break; } return 0; }
case wmAiEvent:for(const auto& event:events_.drain())for(auto& project:projects_)if(project.runtime->apply(event))break;if(!closing_)animateAiStatus();return 0;
case WM_TIMER:if(closing_)return 0;if(a==77)Terminal::pump();else if(a==aiAnimationTimerId)animateAiStatus();else if(a==1)tick();return 0;
case WM_ACTIVATE:if(closing_)return 0;if(LOWORD(a)==WA_INACTIVE)endProjectDrag();else if(!projects_.empty())PostMessageW(projects_[active_].runtime->window(),WM_APP+12,0,0);return 0;
case WM_SETFOCUS:if(closing_)return 0;if(!projects_.empty())projects_[active_].runtime->activate(true);return 0;
case WM_XBUTTONDOWN:case WM_XBUTTONDBLCLK:navigate(GET_XBUTTON_WPARAM(a)==XBUTTON2);return TRUE;
case WM_XBUTTONUP:return TRUE;
case WM_APPCOMMAND:{const auto command=GET_APPCOMMAND_LPARAM(b);if(command==APPCOMMAND_BROWSER_BACKWARD||command==APPCOMMAND_BROWSER_FORWARD){navigate(command==APPCOMMAND_BROWSER_FORWARD);return TRUE;}break;}
case WM_LBUTTONDOWN:{recent_.close();endProjectDrag();const POINT point{GET_X_LPARAM(b),GET_Y_LPARAM(b)};const auto addButton=addButtonBounds();const int width=px(session_.projectListWidth);if(point.x>=width&&point.x<width+px(projectBorderWidth)){dragging_=true;SetCapture(window_);}else if(PtInRect(&addButton,point))choose();else if(const auto index=hit(point);index<projects_.size()){select(index,point.x<row(index).left+px(30));projectDrag_=ProjectDrag{projects_[index].id,point,point,false,std::nullopt};SetCapture(window_);}return 0;}
case WM_MOUSEMOVE:if(projectDrag_){updateProjectDrag({GET_X_LPARAM(b),GET_Y_LPARAM(b)});return 0;}if(dragging_){session_.projectListWidth=std::clamp(MulDiv(GET_X_LPARAM(b),96,dpi_),100,600);appDirty_=true;appChanged_=GetTickCount64();layout();}return 0;
case WM_LBUTTONUP:if(projectDrag_&&projectDrag_->active)updateProjectDrag({GET_X_LPARAM(b),GET_Y_LPARAM(b)});endProjectDrag(true);dragging_=false;if(GetCapture()==window_)ReleaseCapture();return 0;
case WM_CANCELMODE:endProjectDrag();dragging_=false;if(GetCapture()==window_)ReleaseCapture();return 0;
case WM_CAPTURECHANGED:endProjectDrag();dragging_=false;return 0;
case WM_KEYDOWN:if(a==VK_ESCAPE&&projectDrag_){endProjectDrag();return 0;}break;
case WM_RBUTTONDOWN:endProjectDrag();return 0;
case WM_MOUSEWHEEL:{if(GET_KEYSTATE_WPARAM(a)&MK_SHIFT)return 0;POINT point{GET_X_LPARAM(b),GET_Y_LPARAM(b)};ScreenToClient(window_,&point);if(point.x<px(session_.projectListWidth)){wheel_+=GET_WHEEL_DELTA_WPARAM(a);scroll_-=wheel_/WHEEL_DELTA*3;wheel_%=WHEEL_DELTA;layout();if(projectDrag_)updateProjectDrag(point);}return 0;}
case WM_MOUSEHWHEEL:return 0;
case WM_MBUTTONDOWN:close(hit({GET_X_LPARAM(b),GET_Y_LPARAM(b)}));return 0;
case WM_CONTEXTMENU:{endProjectDrag();POINT point{GET_X_LPARAM(b),GET_Y_LPARAM(b)};auto local=point;ScreenToClient(window_,&local);const auto index=hit(local);if(index>=projects_.size()){choose();return 0;}HMENU menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING,1,L"Close project");if(projects_[index].path.wsl())AppendMenuW(menu,MF_STRING,2,L"Reconnect");const auto command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,0,window_,nullptr);DestroyMenu(menu);if(command==1)close(index);else if(command==2){select(index);projects_[index].runtime->revalidate();}return 0;}
case WM_SETCURSOR:{POINT point{};GetCursorPos(&point);ScreenToClient(window_,&point);if(point.x>=px(session_.projectListWidth)&&point.x<px(session_.projectListWidth)+px(5)){SetCursor(LoadCursorW(nullptr,IDC_SIZEWE));return TRUE;}break;}
case WM_NOTIFY:{const auto header=reinterpret_cast<NMHDR*>(b);if(header->hwndFrom==tooltips_&&header->code==TTN_GETDISPINFOW&&header->idFrom>0&&header->idFrom<=projects_.size()){const auto& project=projects_[header->idFrom-1];tipText_=project.path.native()+(project.path.wsl() ? L"\nWSL · "+wide(project.path.location().distro)+L" · "+project.runtime->connectionDescription() : L"")+project.runtime->description();reinterpret_cast<NMTTDISPINFOW*>(b)->lpszText=tipText_.data();}return 0;}
case WM_COPYDATA:{
    const auto data=reinterpret_cast<COPYDATASTRUCT*>(b);
    if(closing_||!data||(data->dwData!=0x50505901&&data->dwData!=startupCopyData)||data->cbData<sizeof(wchar_t)||data->cbData>65536||data->cbData%sizeof(wchar_t)||!data->lpData)return FALSE;
    const auto text=static_cast<const wchar_t*>(data->lpData);const auto length=data->cbData/sizeof(wchar_t)-1;
    if(text[length]||std::find(text,text+length,L'\0')!=text+length)return FALSE;
    if(data->dwData==0x50505901){if(!length)return FALSE;open(fs::path(text));return TRUE;}
    try{
        const auto options=parseStartupCommandLine(text);
        if(pendingProjects_.size()+options.projects.size()>1000)return FALSE;
        for(const auto& project:options.projects)if(!isAbsoluteStartupProjectPath(project))return FALSE;
        // Acknowledge promptly; loading projects and restoring focus run later
        // in our own message loop, outside the sender's synchronous request.
        if(!PostMessageW(window_,wmStartup,0,0))return FALSE;
        for(const auto& project:options.projects)pendingProjects_.emplace_back(project);
        activationPending_=true;
        return TRUE;
    }catch(const std::exception&){return FALSE;}
}
case WM_QUERYENDSESSION:if(!closing_){for(auto& project:projects_)if(!project.runtime->canClose())return FALSE;saveAll();}return TRUE;
case WM_ENDSESSION:if(a&&!closing_){closing_=true;saveAll();for(auto& project:projects_)project.runtime->shutdown();DestroyWindow(window_);}return 0;
case WM_CLOSE:if(closing_)return 0;for(auto& project:projects_)if(!project.runtime->canClose())return 0;finishClose();return 0;
case WM_DESTROY:singleInstance_->unpublish(window_);KillTimer(window_,1);KillTimer(window_,aiAnimationTimerId);errors_.destroy();projects_.clear();if(storageWorker_){storageWorker_->submit(L"release-storage",[this]{storage_.reset();});storageWorker_->drain();storageWorker_->stop();}PostQuitMessage(0);return 0;
}return DefWindowProcW(window_,m,a,b);}
}
}
extern "C" __declspec(dllexport) int WINAPI PoppyMainV1(HINSTANCE instance,int show){
    const HRESULT com=OleInitialize(nullptr);
    int result=1;
    try{
        auto options=poppy::parseStartupCommandLine(GetCommandLineW());
        // Resolve against the launching process's working directory before IPC.
        for(auto& project:options.projects){
            project=poppy::resolveStartupProjectPath(project);
            // Query the original UNC spelling directly, without another
            // std::filesystem interpretation of its root.
            const DWORD attributes=GetFileAttributesW(project.c_str());
            if(attributes==INVALID_FILE_ATTRIBUTES){
                const auto error=GetLastError();
                throw std::runtime_error("The project directory does not exist or cannot be accessed: "+poppy::utf8(project)+"\n\n"+poppy::utf8(poppy::winError(error)));
            }
            if(!(attributes&FILE_ATTRIBUTE_DIRECTORY))throw std::runtime_error("The project path is not a directory: "+poppy::utf8(project));
        }
        poppy::SingleInstance singleInstance(poppy::stateDirectory());
        if(singleInstance.acquireOrForward(options)){poppy::Workspace workspace;result=workspace.run(instance,show,options,singleInstance);}
        else result=0;
    }catch(const std::exception& error){MessageBoxW(nullptr,poppy::wide(error.what()).c_str(),L"Poppy",MB_OK|MB_ICONERROR);}
    catch(...){MessageBoxW(nullptr,L"Poppy could not start.",L"Poppy",MB_OK|MB_ICONERROR);}
    if(SUCCEEDED(com))OleUninitialize();
    return result;
}
