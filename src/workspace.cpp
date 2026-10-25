#include "project_runtime.h"
#include "storage.h"
#include "terminal.h"
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
fs::path stateDirectory() {
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", nullptr, 0);
    if (length) { std::wstring path(length, 0); const auto count = GetEnvironmentVariableW(L"APPDATA", path.data(), length); if (!count || count >= length) throw std::runtime_error("Cannot read APPDATA"); path.resize(count); return fs::path(path) / L"yumayo" / L"Poppy"; }
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &value))) throw std::runtime_error("Cannot locate application data");
    fs::path path(value); CoTaskMemFree(value); return path / L"yumayo" / L"Poppy";
}
struct OpenProject {
    uint64_t id = 0;
    fs::path path;
    std::unique_ptr<ProjectRuntime> runtime;
    bool missing = false, storageAvailable = true, dirty = false;
    ULONGLONG used = 0, changedAt = 0;
    uint64_t statRequest = 0;
};
class Workspace {
public:
    int run(HINSTANCE instance, int show, fs::path initial);
private:
    static LRESULT CALLBACK procedure(HWND,UINT,WPARAM,LPARAM);
    LRESULT handle(UINT,WPARAM,LPARAM);
    void create();
    void layout();
    void paint();
    void choose();
    void open(fs::path path, bool select = true);
    void select(size_t index, bool attention = false);
    void close(size_t index);
    void tick();
    void save(OpenProject& project);
    void saveApp();
    void saveAll();
    void trimDocuments();
    void checkFolder(OpenProject& project);
    void markDirty();
    void updateTools();
    size_t hit(POINT point) const;
    RECT row(size_t index) const;
    OpenProject* find(uint64_t id);
    std::wstring subtitle(size_t index) const;
    void report(std::string error);
    HWND window_ = nullptr, tooltips_ = nullptr;
    HINSTANCE instance_ = nullptr;
    Renderer renderer_;
    HookServer hooks_;
    std::unique_ptr<Worker> storageWorker_;
    // This connection is created, used and destroyed on storageWorker_ only.
    std::unique_ptr<Storage> storage_;
    std::vector<OpenProject> projects_;
    AppSession session_;
    fs::path initial_;
    size_t active_ = 0, tools_ = 0;
    uint64_t nextId_ = 1;
    UINT dpi_ = 96;
    int scroll_ = 0, wheel_ = 0;
    bool dragging_ = false, restoring_ = true, appDirty_ = false, closing_ = false;
    ULONGLONG appChanged_ = 0;
    std::wstring tipText_, storageError_;
    int px(int value) const { return MulDiv(value,dpi_,96); }
};
OpenProject* Workspace::find(uint64_t id) { const auto found=std::find_if(projects_.begin(),projects_.end(),[&](const OpenProject& project){return project.id==id;});return found==projects_.end()?nullptr:&*found; }
int Workspace::run(HINSTANCE instance,int show,fs::path initial) {
    initializeRendering();instance_=instance;initial_=std::move(initial);
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_WIN95_CLASSES};InitCommonControlsEx(&common);
    WNDCLASSW cls{};cls.lpfnWndProc=procedure;cls.hInstance=instance;cls.lpszClassName=L"PoppyMain";cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hIcon=LoadIconW(nullptr,IDI_APPLICATION);RegisterClassW(&cls);
    window_=CreateWindowExW(0,cls.lpszClassName,L"Poppy",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,1380,820,nullptr,nullptr,instance,this);
    if(!window_)return 1;
    ShowWindow(window_,show);UpdateWindow(window_);
    MSG message{};
    while(GetMessageW(&message,nullptr,0,0)>0) {
        const bool key=message.message==WM_KEYDOWN;
        if(key && (GetKeyState(VK_CONTROL)&0x8000) && message.wParam=='O'){choose();continue;}
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
    dpi_=GetDpiForWindow(window_);renderer_.setDpi(dpi_);
    const BOOL dark=TRUE;DwmSetWindowAttribute(window_,20,&dark,sizeof(dark));
    tooltips_=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,window_,nullptr,instance_,nullptr);
    SendMessageW(tooltips_,TTM_SETMAXTIPWIDTH,0,700);
    hooks_.start(window_);
    storageWorker_=std::make_unique<Worker>(window_);
    storageWorker_->submit(L"load-app",[this]{
        AppSession state;std::string error;
        try{storage_=std::make_unique<Storage>(stateDirectory());state=storage_->loadApp();}catch(const std::exception& failure){error=failure.what();storage_.reset();}
        storageWorker_->complete([this,state=std::move(state),error]{
            session_=state;restoring_=false;
            if(session_.positioned){
                RECT rect{session_.x,session_.y,session_.x+session_.width,session_.y+session_.height};MONITORINFO monitor{};monitor.cbSize=sizeof(monitor);
                if(GetMonitorInfoW(MonitorFromRect(&rect,MONITOR_DEFAULTTONEAREST),&monitor)){
                    const int width=std::min(rect.right-rect.left,monitor.rcWork.right-monitor.rcWork.left),height=std::min(rect.bottom-rect.top,monitor.rcWork.bottom-monitor.rcWork.top);
                    rect.left=std::clamp(rect.left,monitor.rcWork.left,monitor.rcWork.right-width);rect.top=std::clamp(rect.top,monitor.rcWork.top,monitor.rcWork.bottom-height);
                    SetWindowPos(window_,nullptr,rect.left,rect.top,width,height,SWP_NOZORDER|SWP_NOACTIVATE);
                }
                if(session_.maximized)ShowWindow(window_,SW_MAXIMIZE);
            }
            const auto paths=session_.openProjects;
            if (!initial_.empty()) open(initial_, false);
            else if (std::find(paths.begin(), paths.end(), session_.lastProject) != paths.end()) open(fs::u8path(session_.lastProject), false);
            for(const auto& path:paths)open(fs::u8path(path),false);
            std::map<std::wstring, size_t> order;
            for (size_t i = 0; i < paths.size(); ++i) order[pathKey(fs::u8path(paths[i]))] = i;
            std::stable_sort(projects_.begin(), projects_.end(), [&](const OpenProject& a, const OpenProject& b) {
                const auto left = order.find(pathKey(a.path)), right = order.find(pathKey(b.path));
                return (left == order.end() ? paths.size() : left->second) < (right == order.end() ? paths.size() : right->second);
            });
            if(!session_.lastProject.empty())for(size_t i=0;i<projects_.size();++i)if(pathKey(projects_[i].path)==pathKey(fs::u8path(session_.lastProject))){active_=i;break;}
            if(!initial_.empty())open(initial_);
            else if(!projects_.empty())select(active_);
            layout();if(!error.empty())report(error);
        });
    });
    SetTimer(window_,1,80,nullptr);
}
void Workspace::choose(){
    IFileOpenDialog* dialog=nullptr;if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog))))return;
    DWORD options=0;dialog->GetOptions(&options);dialog->SetOptions(options|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST);dialog->SetTitle(L"Add project folder");
    if(SUCCEEDED(dialog->Show(window_))){IShellItem* item=nullptr;if(SUCCEEDED(dialog->GetResult(&item))){PWSTR path=nullptr;if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))){open(fs::path(path));CoTaskMemFree(path);}item->Release();}}
    dialog->Release();
}
void Workspace::open(fs::path path,bool shouldSelect){
    if (restoring_) { initial_ = std::move(path); return; }
    std::error_code error;path=fs::absolute(path,error).lexically_normal().make_preferred();if(error)return;
    const auto key=pathKey(path);
    for(size_t i=0;i<projects_.size();++i)if(pathKey(projects_[i].path)==key){if(shouldSelect)select(i);return;}
    if (projects_.size() >= 1000) return;
    OpenProject project;project.id=nextId_++;project.path=path;project.runtime=std::make_unique<ProjectRuntime>();
    project.runtime->openFolder=[this](const fs::path& folder){open(folder);};
    project.runtime->changed=[this]{InvalidateRect(window_,nullptr,FALSE);};
    project.runtime->create(window_,instance_,&hooks_);project.runtime->setDpi(dpi_);
    const auto id=project.id;projects_.push_back(std::move(project));
    if(shouldSelect)select(projects_.size()-1);else layout();
    storageWorker_->submit(L"load:"+std::to_wstring(id),[this,id,path,key]{
        std::optional<ProjectSession> state;std::string failure;std::error_code folderError;
        const bool missing=!fs::is_directory(path,folderError);
        try{if(storage_)state=storage_->loadProject(utf8(key),path.u8string());}catch(const std::exception& error){failure=error.what();}
        if (state && !missing) {
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
            auto project=find(id);if(!project)return;
            project->missing=missing;project->storageAvailable=failure.empty();
            project->runtime->initialize(path,state.value_or(ProjectSession{}),state.has_value());
            project->dirty=true;project->changedAt=GetTickCount64();
            if(project==&projects_[active_]){layout();project->runtime->activate(true);}
            updateTools();InvalidateRect(window_,nullptr,FALSE);if(!failure.empty())report(failure);
        });
    });
    appDirty_=true;appChanged_=GetTickCount64();
}
void Workspace::select(size_t index,bool attention){
    if(index>=projects_.size())return;
    if(active_<projects_.size()&&active_!=index){auto& previous=projects_[active_];previous.runtime->activate(false);previous.dirty=true;previous.changedAt=GetTickCount64();}
    active_=index;projects_[index].used=GetTickCount64();
    for(size_t i=0;i<projects_.size();++i)if(i!=active_)ShowWindow(projects_[i].runtime->window(),SW_HIDE);
    layout();projects_[active_].runtime->activate(true);if(attention)projects_[active_].runtime->focusAttention();
    const int count=std::max(1,static_cast<int>(projects_.size()));
    const auto rect=row(index);RECT client{};GetClientRect(window_,&client);
    if(rect.top<px(44))scroll_=std::min<int>(static_cast<int>(index),count-1);
    else if(rect.bottom>client.bottom-px(44))scroll_=std::max(0,static_cast<int>(index)-(static_cast<int>(client.bottom)-px(88))/px(48)+1);
    SetWindowTextW(window_,(L"Poppy — "+projects_[active_].path.native()).c_str());
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
void Workspace::save(OpenProject& project){
    if(!project.runtime->ready()||!project.storageAvailable)return;
    const auto state=project.runtime->snapshot();const auto path=project.path;const auto key=utf8(pathKey(path));
    storageWorker_->submit(L"save:"+std::to_wstring(project.id),[this,state,path,key]{
        try{if(storage_)storage_->saveProject(key,path.u8string(),state);}catch(const std::exception& error){const std::string text=error.what();storageWorker_->complete([this,text]{report(text);});}
    });project.dirty=false;
}
void Workspace::saveApp(){
    if(restoring_)return;
    session_.openProjects.clear(); session_.openProjectKeys.clear();
    for(const auto& project:projects_) { session_.openProjects.push_back(project.path.u8string()); session_.openProjectKeys.push_back(utf8(pathKey(project.path))); }
    session_.lastProject=projects_.empty()?std::string{}:projects_[active_].path.u8string();
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
void Workspace::close(size_t index){
    if(index>=projects_.size() || !projects_[index].runtime->canClose())return;
    save(projects_[index]);projects_.erase(projects_.begin()+static_cast<std::ptrdiff_t>(index));
    if(index<active_)--active_;
    if(!projects_.empty())select(std::min(active_,projects_.size()-1));else{active_=0;layout();}
    appDirty_=true;appChanged_=GetTickCount64();updateTools();InvalidateRect(window_,nullptr,FALSE);
}
void Workspace::checkFolder(OpenProject& project) {
    if (!project.runtime->ready()) return;
    const auto path = project.path; const auto id = project.id; const auto request = ++project.statRequest;
    storageWorker_->submit(L"stat:" + std::to_wstring(id), [this, path, id, request] {
        std::error_code error; const bool missing = !fs::is_directory(path, error);
        storageWorker_->complete([this, id, request, missing] {
            const auto project = find(id); if (!project || project->statRequest != request) return;
            const bool restored = project->missing && !missing; project->missing = missing;
            if (restored) project->runtime->revalidate();
            InvalidateRect(window_, nullptr, FALSE);
        });
    });
}
void Workspace::markDirty(){if(!projects_.empty()){projects_[active_].dirty=true;projects_[active_].changedAt=GetTickCount64();}}
void Workspace::tick(){
    const auto now=GetTickCount64();bool animate=false;
    for(size_t i=0;i<projects_.size();++i){auto& project=projects_[i];project.runtime->poll();if(project.dirty&&now-project.changedAt>=650)save(project);
        RECT r{};GetClientRect(window_,&r);const auto item=row(i);if(item.bottom>px(44)&&item.top<r.bottom-px(44)&&project.runtime->summary().state==AiState::Running)animate=true;
    }
    trimDocuments();
    if(appDirty_&&now-appChanged_>=650)saveApp();
    if(animate&&!IsIconic(window_)&&IsWindowVisible(window_)) {RECT r{};GetClientRect(window_,&r);r.right=px(session_.projectListWidth);InvalidateRect(window_,&r,FALSE);}
}
RECT Workspace::row(size_t index) const {const int top=px(44)+(static_cast<int>(index)-scroll_)*px(48);return {0,top,px(session_.projectListWidth),top+px(48)};}
size_t Workspace::hit(POINT point) const {RECT r{};GetClientRect(window_,&r);if(point.x<0||point.x>=px(session_.projectListWidth)||point.y<px(44)||point.y>=r.bottom-px(44))return projects_.size();const auto index=scroll_+(point.y-px(44))/px(48);return index>=0?static_cast<size_t>(index):projects_.size();}
std::wstring Workspace::subtitle(size_t index) const {
    const auto& path=projects_[index].path;const auto name=path.filename().native();bool duplicate=false;
    for(size_t i=0;i<projects_.size();++i)if(i!=index&&CompareStringOrdinal(name.c_str(),-1,projects_[i].path.filename().c_str(),-1,TRUE)==CSTR_EQUAL)duplicate=true;
    if(!duplicate)return projects_[index].missing?L"Folder not found":L"";
    size_t depth=1;auto suffix=[](fs::path parent,size_t count){fs::path result;while(count--&&!parent.empty()){result=parent.filename()/result;const auto next=parent.parent_path();if(next==parent){result=parent.root_path()/result;break;}parent=next;}return result.native();};
    for(;;++depth){auto text=suffix(path.parent_path(),depth);bool unique=true;for(size_t i=0;i<projects_.size();++i)if(i!=index&&path.filename()==projects_[i].path.filename()&&text==suffix(projects_[i].path.parent_path(),depth))unique=false;if(unique||depth>32)return text;}
}
void Workspace::layout(){
    if(!window_)return;
    RECT r{};GetClientRect(window_,&r);const int left=px(session_.projectListWidth)+px(5);
    const int page=std::max(1,(static_cast<int>(r.bottom)-px(88))/px(48));scroll_=std::clamp(scroll_,0,std::max(0,static_cast<int>(projects_.size())-page));
    if(!projects_.empty())SetWindowPos(projects_[active_].runtime->window(),nullptr,left,0,std::max(0L,r.right-left),r.bottom,SWP_NOZORDER|SWP_NOACTIVATE);
    updateTools();InvalidateRect(window_,nullptr,FALSE);
}
void Workspace::updateTools(){
    for(size_t i=0;i<tools_;++i){TOOLINFOW tip{};tip.cbSize=sizeof(tip);tip.hwnd=window_;tip.uId=i+1;SendMessageW(tooltips_,TTM_DELTOOLW,0,reinterpret_cast<LPARAM>(&tip));}
    tools_=projects_.size();RECT client{};GetClientRect(window_,&client);
    for(size_t i=0;i<projects_.size();++i){auto rect=row(i);rect.top=std::max<LONG>(px(44),rect.top);rect.bottom=std::min<LONG>(client.bottom-px(44),rect.bottom);if(rect.top>=rect.bottom)continue;
        TOOLINFOW tip{};tip.cbSize=sizeof(tip);tip.hwnd=window_;tip.uId=i+1;tip.uFlags=TTF_SUBCLASS;tip.rect=rect;tip.lpszText=LPSTR_TEXTCALLBACKW;SendMessageW(tooltips_,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));}
}
void Workspace::paint(){
    Renderer::Paint paint(renderer_,window_);if(!paint)return;RECT client{};GetClientRect(window_,&client);renderer_.fill(client,background);
    const int width=px(session_.projectListWidth);renderer_.fill({width,0,width+px(5),client.bottom},border);
    renderer_.label({px(12),0,width-px(8),px(44)},L"PROJECTS",foreground);
    for(size_t i=0;i<projects_.size();++i){auto rect=row(i);if(rect.top<px(44)||rect.bottom>client.bottom-px(44))continue;
        const auto& project=projects_[i];const auto summary=project.runtime->summary();renderer_.fill(rect,i==active_?panel:background);if(i==active_)renderer_.fill({0,rect.top,px(3),rect.bottom},accent);
        drawAiStatus(renderer_,{px(4),rect.top,px(30),rect.bottom},summary.state,dpi_,static_cast<unsigned>(GetTickCount64()/80));
        const auto secondary=subtitle(i);const auto title=project.path.filename().empty()?project.path.native():project.path.filename().native();
        renderer_.label({px(32),rect.top,width-px(22),rect.top+(secondary.empty()?px(48):px(27))},title,project.missing?muted:foreground);
        if(!secondary.empty())renderer_.label({px(32),rect.top+px(24),width-px(8),rect.bottom},secondary,muted);
        if(summary.unread)renderer_.fill({width-px(12),rect.top+px(10),width-px(7),rect.top+px(15)},accent);
        const auto running=summary.counts[static_cast<size_t>(AiState::Running)];if(running&&summary.state!=AiState::Running)renderer_.label({width-px(24),rect.top+px(22),width-px(3),rect.bottom},std::to_wstring(running),accent);
    }
    renderer_.fill({0,client.bottom-px(44),width,client.bottom},panel);renderer_.label({px(12),client.bottom-px(44),width-px(8),client.bottom},L"+  Open folder",accent);
    if(projects_.empty())renderer_.label({width+px(30),px(40),client.right-px(30),client.bottom},restoring_?L"Restoring projects…":L"Open a folder with Ctrl+O.",muted,DT_LEFT|DT_TOP|DT_WORDBREAK);
}
void Workspace::report(std::string error){storageError_=wide(error);if(!closing_)MessageBoxW(window_,(L"Could not read or save state.\n\n"+storageError_).c_str(),L"Poppy",MB_OK|MB_ICONWARNING);}
LRESULT CALLBACK Workspace::procedure(HWND w,UINT m,WPARAM a,LPARAM b){auto self=reinterpret_cast<Workspace*>(GetWindowLongPtrW(w,GWLP_USERDATA));if(m==WM_NCCREATE){self=static_cast<Workspace*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams);self->window_=w;SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}return self?self->handle(m,a,b):DefWindowProcW(w,m,a,b);}
LRESULT Workspace::handle(UINT m,WPARAM a,LPARAM b){switch(m){
case WM_CREATE:create();return 0;case WM_PAINT:paint();return 0;case WM_ERASEBKGND:return 1;case WM_SIZE:layout();if(!restoring_){appDirty_=true;appChanged_=GetTickCount64();}return 0;
case WM_GETMINMAXINFO:reinterpret_cast<MINMAXINFO*>(b)->ptMinTrackSize={px(900),px(480)};return 0;
case WM_DPICHANGED:{dpi_=HIWORD(a);renderer_.setDpi(dpi_);for(auto& project:projects_)project.runtime->setDpi(dpi_);const auto rect=reinterpret_cast<RECT*>(b);SetWindowPos(window_,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER|SWP_NOACTIVATE);layout();return 0;}
case wmWork:storageWorker_->dispatch();return 0;
case wmAiEvent:for(const auto& event:hooks_.drain())for(auto& project:projects_)if(project.runtime->apply(event))break;return 0;
case WM_TIMER:if(a==77)Terminal::pump();else tick();return 0;
case WM_ACTIVATE:if(LOWORD(a)!=WA_INACTIVE&&!projects_.empty())PostMessageW(projects_[active_].runtime->window(),WM_APP+12,0,0);return 0;
case WM_SETFOCUS:if(!projects_.empty())projects_[active_].runtime->activate(true);return 0;
case WM_LBUTTONDOWN:{const POINT point{GET_X_LPARAM(b),GET_Y_LPARAM(b)};RECT r{};GetClientRect(window_,&r);const int width=px(session_.projectListWidth);if(point.x>=width&&point.x<width+px(5)){dragging_=true;SetCapture(window_);}else if(point.x<width&&point.y>=r.bottom-px(44))choose();else if(const auto index=hit(point);index<projects_.size())select(index,point.x<px(30));return 0;}
case WM_MOUSEMOVE:if(dragging_){session_.projectListWidth=std::clamp(MulDiv(GET_X_LPARAM(b),96,dpi_),100,600);appDirty_=true;appChanged_=GetTickCount64();layout();}return 0;
case WM_LBUTTONUP:case WM_CANCELMODE:dragging_=false;if(GetCapture()==window_)ReleaseCapture();return 0;case WM_CAPTURECHANGED:dragging_=false;return 0;
case WM_MOUSEWHEEL:{POINT point{GET_X_LPARAM(b),GET_Y_LPARAM(b)};ScreenToClient(window_,&point);if(point.x<px(session_.projectListWidth)){wheel_+=GET_WHEEL_DELTA_WPARAM(a);scroll_-=wheel_/WHEEL_DELTA*3;wheel_%=WHEEL_DELTA;layout();}return 0;}
case WM_MBUTTONDOWN:close(hit({GET_X_LPARAM(b),GET_Y_LPARAM(b)}));return 0;
case WM_CONTEXTMENU:{POINT point{GET_X_LPARAM(b),GET_Y_LPARAM(b)};auto local=point;ScreenToClient(window_,&local);const auto index=hit(local);if(index>=projects_.size())break;HMENU menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING,1,L"Close project");const auto command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,0,window_,nullptr);DestroyMenu(menu);if(command==1)close(index);return 0;}
case WM_SETCURSOR:{POINT point{};GetCursorPos(&point);ScreenToClient(window_,&point);if(point.x>=px(session_.projectListWidth)&&point.x<px(session_.projectListWidth)+px(5)){SetCursor(LoadCursorW(nullptr,IDC_SIZEWE));return TRUE;}break;}
case WM_NOTIFY:{const auto header=reinterpret_cast<NMHDR*>(b);if(header->hwndFrom==tooltips_&&header->code==TTN_GETDISPINFOW&&header->idFrom>0&&header->idFrom<=projects_.size()){const auto& project=projects_[header->idFrom-1];tipText_=project.path.native()+project.runtime->description();reinterpret_cast<NMTTDISPINFOW*>(b)->lpszText=tipText_.data();}return 0;}
case WM_COPYDATA:{const auto data=reinterpret_cast<COPYDATASTRUCT*>(b);if(data->dwData!=0x50505901||data->cbData<sizeof(wchar_t)||data->cbData>65536||data->cbData%sizeof(wchar_t)||!data->lpData)return FALSE;const auto text=static_cast<const wchar_t*>(data->lpData);if(text[data->cbData/sizeof(wchar_t)-1])return FALSE;open(fs::path(text));return TRUE;}
case WM_QUERYENDSESSION:if(!closing_){for(auto& project:projects_)if(!project.runtime->canClose())return FALSE;saveAll();}return TRUE;
case WM_ENDSESSION:if(a&&!closing_){closing_=true;saveAll();for(auto& project:projects_)project.runtime->shutdown();DestroyWindow(window_);}return 0;
case WM_CLOSE:if(closing_)return 0;for(auto& project:projects_)if(!project.runtime->canClose())return 0;closing_=true;saveAll();for(auto& project:projects_)project.runtime->shutdown();if(!storageError_.empty()){MessageBoxW(window_,(L"Some state could not be saved.\n\n"+storageError_).c_str(),L"Poppy",MB_OK|MB_ICONWARNING);}closing_=true;DestroyWindow(window_);return 0;
case WM_DESTROY:KillTimer(window_,1);hooks_.stop();projects_.clear();storageWorker_->submit(L"release-storage",[this]{storage_.reset();});storageWorker_->drain();storageWorker_->stop();PostQuitMessage(0);return 0;
}return DefWindowProcW(window_,m,a,b);}
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show){
    const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);poppy::fs::path initial;if(argv&&argc>1)initial=argv[1];if(argv)LocalFree(argv);
    int result=1;try{poppy::Workspace workspace;result=workspace.run(instance,show,std::move(initial));}catch(const std::exception& error){MessageBoxW(nullptr,poppy::wide(error.what()).c_str(),L"Poppy",MB_OK|MB_ICONERROR);}if(SUCCEEDED(com))CoUninitialize();return result;
}
