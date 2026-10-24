#include "terminal_pane.h"
#include <commctrl.h>
#include <algorithm>
#include <cstdlib>

namespace poppy {
TerminalPane::~TerminalPane() { stop(); sessions_.clear(); }
void TerminalPane::create(HWND parent,HINSTANCE instance) {
    instance_ = instance;
    WNDCLASSW cls{}; cls.lpfnWndProc = procedure; cls.hInstance = instance; cls.lpszClassName = L"PoppyTerminalPane"; cls.hCursor = LoadCursorW(nullptr,IDC_ARROW); RegisterClassW(&cls);
    window_ = CreateWindowExW(0,cls.lpszClassName,L"Terminals",WS_CHILD|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,0,0,0,0,parent,nullptr,instance,this);
    tooltips_ = CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,window_,nullptr,instance,nullptr);
    SendMessageW(tooltips_, TTM_SETMAXTIPWIDTH, 0, 700);
    add(); setDpi(GetDpiForWindow(parent));
}
void TerminalPane::add(SavedTerminal configuration) {
    if (sessions_.size() >= 100) return;
    if (configuration.id.empty() || std::any_of(sessions_.begin(),sessions_.end(),[&](const auto& s){return s->configuration.id==configuration.id;})) configuration.id = newSessionId();
    if (configuration.name.empty()) configuration.name = "Shell";
    if (configuration.directory.empty()) configuration.directory = directory_.u8string();
    auto session = std::make_unique<TerminalSession>(); session->configuration = std::move(configuration);
    session->terminal = std::make_unique<Terminal>(); session->terminal->create(window_,instance_);
    session->terminal->setDpi(dpi_); session->terminal->setDirectory(fs::u8path(session->configuration.directory));
    auto ptr = session.get();
    session->terminal->starting = [this,ptr] {
        ptr->ai = {}; ptr->notificationId = newSessionId();
        ptr->terminal->trackContainer(ptr->notificationId);
        if (hooks_ && !hooks_->url().empty()) {
            hooks_->add(ptr->notificationId);
            ptr->terminal->setEnvironment({{L"WPF_TASKBAR_URL",wide(hooks_->url())},{L"WPF_TASKBAR_SESSION_ID",wide(ptr->notificationId)}});
        } else ptr->terminal->setEnvironment({{L"WPF_TASKBAR_URL", L""}, {L"WPF_TASKBAR_SESSION_ID", L""}});
        if (changed) changed();
    };
    session->terminal->ended = [this,ptr] {
        const bool registered = !ptr->notificationId.empty();
        if (hooks_ && registered) hooks_->remove(ptr->notificationId);
        ptr->notificationId.clear();
        if (ptr->ai.detected && ptr->ai.state != AiState::Unknown) { ptr->ai.state = AiState::Unknown; ptr->ai.source = AiSource::Process; ptr->ai.confidence = AiConfidence::Unknown; ptr->ai.changedAt = GetTickCount64(); ptr->ai.activity.clear(); ptr->ai.unread = false; }
        if (registered) { InvalidateRect(window_, nullptr, FALSE); if (changed) changed(); }
    };
    sessions_.push_back(std::move(session)); select(sessions_.size()-1);
}
void TerminalPane::setDpi(UINT dpi) { dpi_=dpi; renderer_.setDpi(dpi); header_=MulDiv(32,dpi,96); for(auto& s:sessions_) s->terminal->setDpi(dpi); layout(); }
void TerminalPane::setDirectory(const fs::path& directory) { directory_=directory; for(auto& s:sessions_) if(s->configuration.directory.empty()) {s->configuration.directory=directory.u8string();s->terminal->setDirectory(directory);} }
void TerminalPane::restore(const ProjectSession& state) {
    stop(); for(auto& s:sessions_) DestroyWindow(s->terminal->window()); sessions_.clear();
    for(const auto& saved:state.terminals) add(saved);
    if(sessions_.empty()) add();
    const auto found=std::find_if(sessions_.begin(),sessions_.end(),[&](const auto& s){return s->configuration.id==state.activeTerminal;});
    select(found==sessions_.end()?0:static_cast<size_t>(found-sessions_.begin()));
}
void TerminalPane::snapshot(ProjectSession& state) const { state.terminals.clear();for(const auto& s:sessions_)state.terminals.push_back(s->configuration);state.activeTerminal=active().configuration.id; }
bool TerminalPane::apply(const AiEvent& event) {
    for(size_t i=0;i<sessions_.size();++i) if(sessions_[i]->notificationId==event.sessionId) {
        const bool visible=i==active_ && IsWindowVisible(window_) && GetForegroundWindow()==GetAncestor(window_,GA_ROOT) && !IsIconic(GetAncestor(window_,GA_ROOT));
        sessions_[i]->ai.apply(event,GetTickCount64(),visible); InvalidateRect(window_,nullptr,FALSE); if(changed)changed();return true;
    }
    return false;
}
void TerminalPane::focusAttention() {
    const auto state = summary().state;
    for (size_t i = 0; i < sessions_.size(); ++i) if (sessions_[i]->ai.detected && sessions_[i]->ai.state == state) { select(i); break; }
    SetFocus(window());
}
void TerminalPane::acknowledge() { if(IsWindowVisible(window_) && GetForegroundWindow()==GetAncestor(window_,GA_ROOT) && !IsIconic(GetAncestor(window_,GA_ROOT))) active().ai.acknowledge(); }
AiSummary TerminalPane::summary() const { std::vector<AiStatus> states; for(const auto& s:sessions_)states.push_back(s->ai);return summarizeAi(states); }
std::wstring TerminalPane::description() const {
    std::wstring text;
    const auto aggregate = summary();
    for (size_t i = 0; i < aggregate.counts.size(); ++i) if (aggregate.counts[i]) text += L" " + wide(aiStateName(static_cast<AiState>(i))) + L": " + std::to_wstring(aggregate.counts[i]);
    for(const auto& s:sessions_) {
        text+=L"\n"+wide(s->configuration.name)+L": "+wide(aiStateName(s->ai.state));
        if(s->ai.changedAt) text+=L" · "+std::to_wstring((GetTickCount64()-s->ai.changedAt)/1000)+L"s ago";
        if (s->terminal->cleanupUnconfirmed()) text += L" · container cleanup unconfirmed";
        text+=s->notificationId.empty()?L" · not connected":s->ai.hookObserved?L" · hook / confirmed":L" · awaiting hook";
        if(!s->ai.activity.empty())text+=L"\n  "+wide(s->ai.activity);
    }
    if(hooks_ && !hooks_->error().empty())text+=L"\n"+hooks_->error();
    return text;
}
void TerminalPane::select(size_t index) { if(index>=sessions_.size())return;active_=index;layout();acknowledge();if(changed)changed(); }
void TerminalPane::close(size_t index) {
    if(index>=sessions_.size())return;
    sessions_[index]->terminal->stop();DestroyWindow(sessions_[index]->terminal->window());sessions_.erase(sessions_.begin()+static_cast<std::ptrdiff_t>(index));
    if(index<active_)--active_;
    if(sessions_.empty())add(); else select(std::min(active_,sessions_.size()-1));
}
void TerminalPane::stop(){for(auto& s:sessions_)s->terminal->stop();}
std::vector<RECT> TerminalPane::bounds() const {
    std::vector<RECT> result;int x=-offset_; const int padding=MulDiv(74,dpi_,96);
    for(const auto& s:sessions_){const int width=std::max(MulDiv(110,dpi_,96),renderer_.textWidth(wide(s->configuration.name))+padding);result.push_back({x,0,x+width,header_});x+=width;}
    return result;
}
void TerminalPane::layout(){
    if(!window_||sessions_.empty())return;
    RECT r{};GetClientRect(window_,&r);
    auto rects=bounds();const int edge=std::max(0L,r.right-MulDiv(34,dpi_,96));
    if(rects[active_].left<0)offset_+=rects[active_].left;
    else if(rects[active_].right>edge)offset_+=rects[active_].right-edge;
    offset_=std::max(0,offset_);rects=bounds();
    for(size_t i=0;i<sessions_.size();++i){
        const auto w=sessions_[i]->terminal->window();
        if(i!=active_)ShowWindow(w,SW_HIDE);
        else SetWindowPos(w,nullptr,0,header_,r.right,std::max(0L,r.bottom-header_),SWP_NOZORDER|SWP_NOACTIVATE|SWP_SHOWWINDOW);
    }
    for(size_t i=0;i<tools_;++i){TOOLINFOW tip{};tip.cbSize=sizeof(tip);tip.hwnd=window_;tip.uId=i+1;SendMessageW(tooltips_,TTM_DELTOOLW,0,reinterpret_cast<LPARAM>(&tip));}
    tools_=rects.size();
    for(size_t i=0;i<rects.size();++i){TOOLINFOW tip{};tip.cbSize=sizeof(tip);tip.hwnd=window_;tip.uId=i+1;tip.uFlags=TTF_SUBCLASS;tip.rect=rects[i];tip.rect.left=std::max(0L,tip.rect.left);tip.rect.right=std::min<LONG>(edge,tip.rect.right);tip.lpszText=LPSTR_TEXTCALLBACKW;SendMessageW(tooltips_,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));}
    InvalidateRect(window_,nullptr,FALSE);
}
void TerminalPane::paint(){
    Renderer::Paint paint(renderer_,window_);if(!paint)return;RECT r{};GetClientRect(window_,&r);renderer_.fill(r,background);
    const auto rects=bounds();const int unit=MulDiv(26,dpi_,96),edge=r.right-MulDiv(34,dpi_,96);
    for(size_t i=0;i<rects.size();++i){auto rect=rects[i];rect.right=std::min<LONG>(rect.right,edge);if(rect.left>=rect.right)continue;
        renderer_.fill(rect,i==active_?panel:background);if(i==active_)renderer_.fill({rect.left,header_-2,rect.right,header_},accent);
        drawAiStatus(renderer_,{rect.left,0,rect.left+unit,header_},sessions_[i]->ai.state,dpi_,static_cast<unsigned>(GetTickCount64()/80));
        renderer_.label({rect.left+unit,0,rect.right-unit,header_},wide(sessions_[i]->configuration.name),foreground);
        renderer_.label({rect.right-unit,0,rect.right,header_},L"×",muted,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        if(sessions_[i]->ai.unread)renderer_.fill({rect.left+unit-4,4,rect.left+unit,8},accent);
    }
    renderer_.fill({edge,0,r.right,header_},panel);renderer_.label({edge,0,r.right,header_},L"+",accent,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
}
void TerminalPane::menu(size_t index,POINT point){
    HMENU menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING,1,L"Rename terminal");AppendMenuW(menu,MF_STRING,2,L"Close terminal");
    const auto command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,0,window_,nullptr);DestroyMenu(menu);
    if(command==2)close(index);
    else if(command==1){
        if(rename_)DestroyWindow(rename_);
        renaming_=index;
        rename_=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",wide(sessions_[index]->configuration.name).c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,0,0,0,0,window_,reinterpret_cast<HMENU>(1001),instance_,nullptr);
        SetWindowSubclass(rename_, renameProcedure, 1, reinterpret_cast<DWORD_PTR>(this));
        const auto rect=bounds()[index];SetWindowPos(rename_,HWND_TOP,std::max(0L,rect.left),0,std::max(100L,rect.right-std::max(0L,rect.left)),header_,SWP_SHOWWINDOW);
        SendMessageW(rename_,EM_SETLIMITTEXT,120,0);SendMessageW(rename_,EM_SETSEL,0,-1);SetFocus(rename_);
    }
}
void TerminalPane::addAi(const std::wstring& tool) {
    const DWORD length = GetEnvironmentVariableW(L"POPPY_AI_CONTAINER_DIR", nullptr, 0);
    if (!length) { MessageBoxW(GetAncestor(window_, GA_ROOT), L"Set POPPY_AI_CONTAINER_DIR to the Windows path of your ai-container checkout, then restart Poppy. See ai/README.md.", L"Poppy", MB_OK | MB_ICONINFORMATION); return; }
    std::wstring repository(length, 0); const auto count = GetEnvironmentVariableW(L"POPPY_AI_CONTAINER_DIR", repository.data(), length); if (!count || count >= length) return; repository.resize(count);
    std::wstring executable(32768, 0); const auto size = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size())); if (!size || size >= executable.size()) return; executable.resize(size);
    const auto script = fs::path(executable).parent_path() / L"ai" / L"start-ai.ps1";
    std::error_code error;
    if (!fs::is_regular_file(script, error) || !fs::is_regular_file(fs::path(repository) / L"wsl-containers" / L"wsl-containers.ps1", error)) { MessageBoxW(GetAncestor(window_, GA_ROOT), L"Poppy's ai folder or the ai-container launcher is missing. See ai/README.md.", L"Poppy", MB_OK | MB_ICONWARNING); return; }
    add({"", utf8(tool == L"codex" ? L"Codex" : L"Claude"), directory_.u8string()});
    wchar_t system[MAX_PATH]{}; GetSystemDirectoryW(system, MAX_PATH);
    active().terminal->setCommand({std::wstring(system) + L"\\WindowsPowerShell\\v1.0\\powershell.exe", L"-NoLogo", L"-NoProfile", L"-ExecutionPolicy", L"Bypass", L"-File", script.native(), L"-Repository", repository, L"-Workspace", directory_.native(), L"-Tool", tool});
    active().terminal->launch(); SetFocus(window());
}
void TerminalPane::addMenu(POINT point) {
    HMENU menu = CreatePopupMenu(); AppendMenuW(menu, MF_STRING, 1, L"New shell"); AppendMenuW(menu, MF_STRING, 2, L"New Codex (ai-container)"); AppendMenuW(menu, MF_STRING, 3, L"New Claude Code (ai-container)");
    const auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window_, nullptr); DestroyMenu(menu);
    if (command == 1) { add({"", "Shell", directory_.u8string()}); SetFocus(window()); }
    else if (command == 2 || command == 3) addAi(command == 2 ? L"codex" : L"claude");
}
LRESULT CALLBACK TerminalPane::renameProcedure(HWND window, UINT message, WPARAM a, LPARAM b, UINT_PTR, DWORD_PTR data) {
    auto& self = *reinterpret_cast<TerminalPane*>(data);
    if (message == WM_KEYDOWN && (a == VK_RETURN || a == VK_ESCAPE)) {
        if (a == VK_ESCAPE && self.renaming_ < self.sessions_.size()) SetWindowTextW(window, wide(self.sessions_[self.renaming_]->configuration.name).c_str());
        SetFocus(self.window()); return 0;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, renameProcedure, 1);
    return DefSubclassProc(window, message, a, b);
}
LRESULT CALLBACK TerminalPane::procedure(HWND w,UINT m,WPARAM a,LPARAM b){auto self=reinterpret_cast<TerminalPane*>(GetWindowLongPtrW(w,GWLP_USERDATA));if(m==WM_NCCREATE){self=static_cast<TerminalPane*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams);self->window_=w;SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}return self?self->handle(m,a,b):DefWindowProcW(w,m,a,b);}
LRESULT TerminalPane::handle(UINT m,WPARAM a,LPARAM b){switch(m){
case WM_PAINT:paint();return 0;case WM_ERASEBKGND:return 1;case WM_SIZE:layout();return 0;
case WM_SETFOCUS:SetFocus(window());return 0;
case WM_COMMAND:if(LOWORD(a)==1001&&HIWORD(a)==EN_KILLFOCUS&&rename_){wchar_t text[121]{};GetWindowTextW(rename_,text,121);if(*text&&renaming_<sessions_.size())sessions_[renaming_]->configuration.name=utf8(text);auto edit=std::exchange(rename_,nullptr);DestroyWindow(edit);layout();if(changed)changed();}return 0;
case WM_LBUTTONDOWN:{RECT r{};GetClientRect(window_,&r);const int x=GET_X_LPARAM(b);if(x>=r.right-MulDiv(34,dpi_,96)){add({"","Shell",directory_.u8string()});SetFocus(window());return 0;}const auto rects=bounds();for(size_t i=0;i<rects.size();++i)if(PtInRect(&rects[i],{x,GET_Y_LPARAM(b)})){if(x>=rects[i].right-MulDiv(26,dpi_,96))close(i);else select(i);SetFocus(window());break;}return 0;}
case WM_MOUSEWHEEL:offset_=std::max(0,offset_-GET_WHEEL_DELTA_WPARAM(a)/WHEEL_DELTA*MulDiv(90,dpi_,96));InvalidateRect(window_,nullptr,FALSE);return 0;
case WM_CONTEXTMENU:{POINT point{GET_X_LPARAM(b),GET_Y_LPARAM(b)};if(point.x==-1&&point.y==-1){point={0,header_};ClientToScreen(window_,&point);menu(active_,point);return 0;}auto local=point;ScreenToClient(window_,&local);RECT client{};GetClientRect(window_,&client);if(local.x>=client.right-MulDiv(34,dpi_,96)&&local.y<header_){addMenu(point);return 0;}const auto rects=bounds();for(size_t i=0;i<rects.size();++i)if(PtInRect(&rects[i],local)){menu(i,point);break;}return 0;}
case WM_NOTIFY:if(reinterpret_cast<NMHDR*>(b)->code==TTN_GETDISPINFOW){tooltipText_=description();reinterpret_cast<NMTTDISPINFOW*>(b)->lpszText=tooltipText_.data();}return 0;
}return DefWindowProcW(window_,m,a,b);}
}
