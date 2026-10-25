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
    preview_.create(window_, instance_);
    add(); setDpi(GetDpiForWindow(parent));
}
void TerminalPane::add(SavedTerminal configuration) {
    if (sessions_.size() >= 100) return;
    if (configuration.id.empty() || std::any_of(sessions_.begin(),sessions_.end(),[&](const auto& s){return s->configuration.id==configuration.id;})) configuration.id = newSessionId();
    if (configuration.name.empty()) configuration.name = "Shell";
    if (configuration.directory.empty()) configuration.directory = directory_.u8string();
    auto session = std::make_unique<TerminalSession>(); session->configuration = std::move(configuration);
    session->terminal = std::make_unique<Terminal>(); session->terminal->create(window_,instance_);
    session->terminal->setDpi(dpi_); session->terminal->setDirectory(directory_.wsl() ? windowsProcessDirectory() : fs::u8path(session->configuration.directory));
    if (directory_.wsl()) session->terminal->setDisplayDirectory(wide(session->configuration.directory) + L" · WSL · " + wide(directory_.location().distro));
    auto ptr = session.get();
    session->terminal->focused = [this, ptr] {
        const auto found = std::find_if(sessions_.begin(), sessions_.end(), [ptr](const auto& s) { return s.get() == ptr; });
        if (found != sessions_.end()) { active_ = static_cast<size_t>(found - sessions_.begin()); acknowledge(); InvalidateRect(window_, nullptr, FALSE); }
    };
    session->terminal->starting = [this,ptr] {
        ptr->ai = {}; ptr->notificationId = newSessionId();
        if (directory_.wsl()) {
            ptr->wslToken = newSessionId();
            ptr->terminal->setCommand(provider_->terminalCommand(directory_.at(ptr->configuration.directory), ptr->wslToken, ptr->wslTool));
        } else ptr->terminal->trackContainer(ptr->notificationId);
        if (hooks_ && !hooks_->url().empty()) {
            hooks_->add(ptr->notificationId);
            ptr->terminal->setEnvironment({{L"POPPY_EVENTS_URL",wide(hooks_->url())},{L"POPPY_SESSION_ID",wide(ptr->notificationId)}});
        } else ptr->terminal->setEnvironment({{L"POPPY_EVENTS_URL", L""}, {L"POPPY_SESSION_ID", L""}});
        if (changed) changed();
    };
    session->terminal->stopping = [this, ptr] {
        if (!ptr->wslToken.empty()) {
            const auto token = std::exchange(ptr->wslToken, {}); const bool running = ptr->terminal->running();
            if (provider_ && !provider_->stopTerminal(token) && running) MessageBoxW(GetAncestor(window_, GA_ROOT), L"Could not confirm that this WSL terminal's child processes have exited. Check the processes in this distribution.", L"Poppy — WSL cleanup unconfirmed", MB_OK | MB_ICONWARNING);
        }
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
void TerminalPane::setDirectory(const ResourceId& directory) {
    directory_ = directory;
    for (auto& session : sessions_) {
        if (session->configuration.directory.empty()) session->configuration.directory = directory.u8string();
        session->terminal->setDirectory(directory.wsl() ? windowsProcessDirectory() : fs::u8path(session->configuration.directory));
        if (directory.wsl()) session->terminal->setDisplayDirectory(wide(session->configuration.directory) + L" · WSL · " + wide(directory.location().distro));
    }
}
void TerminalPane::relocate(const ResourceId& source, const ResourceId& destination) {
    directory_ = relocateResource(directory_, source, destination);
    for (auto& session : sessions_) {
        const auto path = relocateResource(directory_.at(session->configuration.directory), source, destination);
        session->configuration.directory = path.u8string(); session->terminal->setDirectory(path.wsl() ? windowsProcessDirectory() : path.local());
        if (path.wsl()) session->terminal->setDisplayDirectory(path.native() + L" · WSL · " + wide(path.location().distro));
    }
}
void TerminalPane::restore(const ProjectSession& state) {
    endDrag(); ratio_ = state.terminalRatio; stop(); for(auto& s:sessions_) DestroyWindow(s->terminal->window()); sessions_.clear();
    for(const auto& saved:state.terminals) add(saved);
    if(sessions_.empty()) add();
    for (size_t i = 0; i < state.terminals.size() && i < sessions_.size(); ++i) { sessions_[i]->configuration.displayed = state.terminals[i].displayed; sessions_[i]->configuration.group = state.terminals[i].group; }
    const auto found=std::find_if(sessions_.begin(),sessions_.end(),[&](const auto& s){return s->configuration.id==state.activeTerminal;});
    select(found==sessions_.end()?0:static_cast<size_t>(found-sessions_.begin()));
}
void TerminalPane::snapshot(ProjectSession& state) const { state.terminals.clear();for(const auto& s:sessions_)state.terminals.push_back(s->configuration);state.activeTerminal=active().configuration.id; state.terminalRatio = ratio_; }
bool TerminalPane::apply(const AiEvent& event) {
    for(size_t i=0;i<sessions_.size();++i) if(sessions_[i]->notificationId==event.sessionId) {
        const bool visible=sessions_[i]->configuration.displayed && IsWindowVisible(window_) && GetForegroundWindow()==GetAncestor(window_,GA_ROOT) && !IsIconic(GetAncestor(window_,GA_ROOT));
        sessions_[i]->ai.apply(event,GetTickCount64(),visible); InvalidateRect(window_,nullptr,FALSE); if(changed)changed();return true;
    }
    return false;
}
void TerminalPane::focusAttention() {
    const auto state = summary().state;
    for (size_t i = 0; i < sessions_.size(); ++i) if (sessions_[i]->ai.detected && sessions_[i]->ai.state == state) { select(i); break; }
    SetFocus(window());
}
void TerminalPane::acknowledge() { if(IsWindowVisible(window_) && GetForegroundWindow()==GetAncestor(window_,GA_ROOT) && !IsIconic(GetAncestor(window_,GA_ROOT))) for (const auto& session : sessions_) if (session->configuration.displayed) session->ai.acknowledge(); }
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
void TerminalPane::select(size_t index) {
    if(index>=sessions_.size())return;
    active_=index; const int group = active().configuration.group;
    for (auto& s : sessions_) if (s->configuration.group == group) s->configuration.displayed = false;
    active().configuration.displayed = true;
    layout(); acknowledge(); if(changed)changed();
}
void TerminalPane::close(size_t index) {
    if(index>=sessions_.size())return;
    endDrag(); if(rename_){DestroyWindow(rename_);rename_=nullptr;}
    sessions_[index]->terminal->stop();DestroyWindow(sessions_[index]->terminal->window());sessions_.erase(sessions_.begin()+static_cast<std::ptrdiff_t>(index));
    if(index<active_)--active_;
    if(sessions_.empty())add(); else select(std::min(active_,sessions_.size()-1));
}
void TerminalPane::stop(){for(auto& s:sessions_)s->terminal->stop();}
size_t TerminalPane::groupTab(int group) const {
    for (size_t i = 0; i < sessions_.size(); ++i) if (sessions_[i]->configuration.group == group && sessions_[i]->configuration.displayed) return i;
    return sessions_.size();
}
void TerminalPane::normalizeGroups() {
    if (std::none_of(sessions_.begin(), sessions_.end(), [](const auto& s) { return s->configuration.group == 0; })) for (auto& s : sessions_) s->configuration.group = 0;
    for (int group = 0; group < 2; ++group) {
        bool selected = false;
        for (auto& s : sessions_) if (s->configuration.group == group && s->configuration.displayed) { if (selected) s->configuration.displayed = false; selected = true; }
        if (!selected) { const auto found = std::find_if(sessions_.begin(), sessions_.end(), [group](const auto& s) { return s->configuration.group == group; }); if (found != sessions_.end()) (*found)->configuration.displayed = true; }
    }
}
RECT TerminalPane::groupBounds(int group) const {
    RECT r{}; GetClientRect(window_, &r);
    const bool split = groupTab(1) < sessions_.size();
    const int width = MulDiv(5, dpi_, 96), middle = MulDiv(std::max(0L, r.right - width), ratio_, 1000);
    return {group ? middle + width : 0, 0, group || !split ? r.right : middle, r.bottom};
}
std::vector<RECT> TerminalPane::bounds() const {
    std::vector<RECT> result; const int padding=MulDiv(74,dpi_,96);
    int x[2] = {groupBounds(0).left - offset_[0], groupBounds(1).left - offset_[1]};
    for(const auto& s:sessions_) { const int group = s->configuration.group; const int width=std::max(MulDiv(110,dpi_,96),renderer_.textWidth(wide(s->configuration.name))+padding); result.push_back({x[group],0,x[group]+width,header_}); x[group]+=width; }
    return result;
}
void TerminalPane::layout(){
    if(!window_||sessions_.empty())return;
    normalizeGroups(); auto rects=bounds();
    for (int group = 0; group < 2; ++group) {
        const auto selected = groupTab(group); if (selected == sessions_.size()) continue;
        const auto pane = groupBounds(group); const int edge = std::max(pane.left, pane.right - MulDiv(34, dpi_, 96));
        if(rects[selected].left < pane.left) offset_[group] += rects[selected].left - pane.left;
        else if(rects[selected].right > edge) offset_[group] += rects[selected].right - edge;
        offset_[group] = std::max(0, offset_[group]);
    }
    rects=bounds();
    for(size_t i=0;i<sessions_.size();++i){
        const auto w=sessions_[i]->terminal->window(); const auto& config = sessions_[i]->configuration;
        const auto pane = groupBounds(config.group);
        if(!config.displayed)ShowWindow(w,SW_HIDE);
        else SetWindowPos(w,nullptr,pane.left,header_,std::max(0L,pane.right-pane.left),std::max(0L,pane.bottom-header_),SWP_NOZORDER|SWP_NOACTIVATE|SWP_SHOWWINDOW);
    }
    const auto left = groupBounds(0), right = groupBounds(1);
    splitter_ = groupTab(1) < sessions_.size() ? RECT{left.right, 0, right.left, left.bottom} : RECT{};
    for(size_t i=0;i<tools_;++i){TOOLINFOW tip{};tip.cbSize=sizeof(tip);tip.hwnd=window_;tip.uId=i+1;SendMessageW(tooltips_,TTM_DELTOOLW,0,reinterpret_cast<LPARAM>(&tip));}
    tools_=rects.size();
    for(size_t i=0;i<rects.size();++i){TOOLINFOW tip{};tip.cbSize=sizeof(tip);tip.hwnd=window_;tip.uId=i+1;tip.uFlags=TTF_SUBCLASS;tip.rect=rects[i];const auto pane = groupBounds(sessions_[i]->configuration.group);tip.rect.left=std::max(pane.left,tip.rect.left);tip.rect.right=std::min<LONG>(pane.right-MulDiv(34,dpi_,96),tip.rect.right);tip.lpszText=LPSTR_TEXTCALLBACKW;if(tip.rect.right>tip.rect.left)SendMessageW(tooltips_,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));}
    InvalidateRect(window_,nullptr,FALSE);
}
void TerminalPane::paint(){
    Renderer::Paint paint(renderer_,window_);if(!paint)return;RECT r{};GetClientRect(window_,&r);renderer_.fill(r,background);
    const auto rects=bounds();const int unit=MulDiv(26,dpi_,96);
    for(size_t i=0;i<rects.size();++i){auto rect=rects[i];const auto pane = groupBounds(sessions_[i]->configuration.group);rect.left=std::max(rect.left,pane.left);rect.right=std::min<LONG>(rect.right,pane.right-MulDiv(34,dpi_,96));if(rect.left>=rect.right)continue;
        const bool selected = sessions_[i]->configuration.displayed;
        renderer_.fill(rect,selected?panel:background);if(selected)renderer_.fill({rect.left,header_-2,rect.right,header_},accent);
        drawAiStatus(renderer_,{rect.left,0,rect.left+unit,header_},sessions_[i]->ai.state,dpi_,static_cast<unsigned>(GetTickCount64()/80));
        renderer_.label({rect.left+unit,0,rect.right-unit,header_},wide(sessions_[i]->configuration.name),foreground);
        renderer_.label({rect.right-unit,0,rect.right,header_},L"×",muted,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        if(sessions_[i]->ai.unread)renderer_.fill({rect.left+unit-4,4,rect.left+unit,8},accent);
    }
    for (int group = 0; group < 2; ++group) if (groupTab(group) < sessions_.size()) {
        const auto pane = groupBounds(group); RECT button{pane.right-MulDiv(34,dpi_,96),0,pane.right,header_}; renderer_.fill(button,panel);renderer_.label(button,L"+",accent,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    }
    renderer_.fill(splitter_, border);
}
void TerminalPane::drag(POINT point) {
    if (!drag_) return;
    auto& d = *drag_; d.point = point;
    if (!d.active) {
        if (std::abs(point.x-d.origin.x)<GetSystemMetricsForDpi(SM_CXDRAG,dpi_) && std::abs(point.y-d.origin.y)<GetSystemMetricsForDpi(SM_CYDRAG,dpi_)) return;
        d.active=true; SendMessageW(tooltips_,TTM_ACTIVATE,FALSE,0);
    }
    RECT client{}; GetClientRect(window_,&client); d.group = -1; d.position = sessions_.size();
    if (!PtInRect(&client,point)) { preview_.hide(); return; }
    const bool split = groupTab(1) < sessions_.size(); const int middle = split ? splitter_.left : client.right/2;
    const int target = point.x < middle ? 0 : 1;
    if (point.y < header_) {
        d.group = split ? target : 0; const auto pane = groupBounds(d.group); const auto rects = bounds();
        LONG x = pane.left;
        for (size_t i=0;i<rects.size();++i) if(sessions_[i]->configuration.group==d.group) {
            if(point.x < (rects[i].left+rects[i].right)/2) { d.position=i; x=rects[i].left; break; }
            d.position=i+1; x=rects[i].right;
        }
        x=std::clamp<LONG>(x,pane.left,std::max(pane.left,pane.right-MulDiv(34,dpi_,96)));
        preview_.show({x,0,x+3,header_}); return;
    }
    if ((!split && sessions_.size()<2) || (split && target==sessions_[d.index]->configuration.group)) { preview_.hide(); return; }
    d.group=target;
    const auto pane = split ? groupBounds(target) : RECT{target?middle:0,0,target?client.right:middle,client.bottom};
    preview_.show({pane.left,header_,pane.right,pane.bottom});
}
void TerminalPane::endDrag(bool drop) {
    if (!drag_) return;
    const auto d = *drag_; drag_.reset(); preview_.hide(); if(GetCapture()==window_)ReleaseCapture();
    SendMessageW(tooltips_,TTM_ACTIVATE,TRUE,0);
    if (!drop || !d.active || d.group<0) return;
    if (groupTab(1)==sessions_.size() && d.group==0 && d.point.y>=header_) for(auto& s:sessions_)s->configuration.group=1;
    auto session=std::move(sessions_[d.index]);session->configuration.group=d.group;session->configuration.displayed=true;
    for(auto& s:sessions_)if(s && s->configuration.group==d.group)s->configuration.displayed=false;
    sessions_.erase(sessions_.begin()+static_cast<std::ptrdiff_t>(d.index));
    size_t position=d.position;if(position>d.index)--position;position=std::min(position,sessions_.size());
    sessions_.insert(sessions_.begin()+static_cast<std::ptrdiff_t>(position),std::move(session));
    active_=position;layout();SetFocus(window());if(changed)changed();
}
bool TerminalPane::key(const MSG& message) {
    if (message.message==WM_KEYDOWN && message.wParam==VK_ESCAPE && (drag_ || resizing_)) { endDrag();resizing_=false;if(GetCapture()==window_)ReleaseCapture();return true; }
    return false;
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
    if (directory_.wsl()) {
        if (!provider_ || provider_->state() != ConnectionState::Ready) { MessageBoxW(GetAncestor(window_, GA_ROOT), L"Reconnect the WSL project before starting an AI CLI.", L"Poppy", MB_OK | MB_ICONINFORMATION); return; }
        const int group = active().configuration.group;
        add({"", utf8(tool == L"codex" ? L"Codex" : L"Claude"), directory_.u8string(), group});
        active().wslTool = tool; active().terminal->launch(); SetFocus(window()); return;
    }
    const DWORD length = GetEnvironmentVariableW(L"POPPY_AI_CONTAINER_DIR", nullptr, 0);
    if (!length) { MessageBoxW(GetAncestor(window_, GA_ROOT), L"Set POPPY_AI_CONTAINER_DIR to the Windows path of your ai-container checkout, then restart Poppy. See ai/README.md.", L"Poppy", MB_OK | MB_ICONINFORMATION); return; }
    std::wstring repository(length, 0); const auto count = GetEnvironmentVariableW(L"POPPY_AI_CONTAINER_DIR", repository.data(), length); if (!count || count >= length) return; repository.resize(count);
    std::wstring executable(32768, 0); const auto size = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size())); if (!size || size >= executable.size()) return; executable.resize(size);
    const auto script = fs::path(executable).parent_path() / L"ai" / L"start-ai.ps1";
    std::error_code error;
    if (!fs::is_regular_file(script, error) || !fs::is_regular_file(fs::path(repository) / L"wsl-containers" / L"wsl-containers.ps1", error)) { MessageBoxW(GetAncestor(window_, GA_ROOT), L"Poppy's ai folder or the ai-container launcher is missing. See ai/README.md.", L"Poppy", MB_OK | MB_ICONWARNING); return; }
    const int group = active().configuration.group;
    add({"", utf8(tool == L"codex" ? L"Codex" : L"Claude"), directory_.u8string(), group});
    wchar_t system[MAX_PATH]{}; GetSystemDirectoryW(system, MAX_PATH);
    active().terminal->setCommand({std::wstring(system) + L"\\WindowsPowerShell\\v1.0\\powershell.exe", L"-NoLogo", L"-NoProfile", L"-ExecutionPolicy", L"Bypass", L"-File", script.native(), L"-Repository", repository, L"-Workspace", directory_.local().native(), L"-Tool", tool});
    active().terminal->launch(); SetFocus(window());
}
void TerminalPane::addMenu(POINT point) {
    HMENU menu = CreatePopupMenu(); AppendMenuW(menu, MF_STRING, 1, L"New shell"); AppendMenuW(menu, MF_STRING, 2, directory_.wsl() ? L"New Codex (WSL)" : L"New Codex (ai-container)"); AppendMenuW(menu, MF_STRING, 3, directory_.wsl() ? L"New Claude Code (WSL)" : L"New Claude Code (ai-container)");
    const auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window_, nullptr); DestroyMenu(menu);
    if (command == 1) { const int group = active().configuration.group; add({"", "Shell", directory_.u8string(), group}); SetFocus(window()); }
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
case WM_LBUTTONDOWN:{
    const POINT point{GET_X_LPARAM(b),GET_Y_LPARAM(b)};
    if (PtInRect(&splitter_,point)) { resizing_=true;SetCapture(window_);return 0; }
    for(int group=0;group<2;++group)if(groupTab(group)<sessions_.size()) {
        const auto pane=groupBounds(group);const RECT button{pane.right-MulDiv(34,dpi_,96),0,pane.right,header_};
        if(PtInRect(&button,point)){add({"","Shell",directory_.u8string(),group});SetFocus(window());return 0;}
    }
    const auto rects=bounds();for(size_t i=0;i<rects.size();++i) {
        auto rect=rects[i];const auto pane=groupBounds(sessions_[i]->configuration.group);rect.left=std::max(rect.left,pane.left);rect.right=std::min<LONG>(rect.right,pane.right-MulDiv(34,dpi_,96));
        if(PtInRect(&rect,point)){if(point.x>=rect.right-MulDiv(26,dpi_,96))close(i);else{select(i);drag_=Drag{i,point,point,false,-1,sessions_.size()};SetCapture(window_);}SetFocus(window());break;}
    }return 0;
}
case WM_MOUSEMOVE:if(drag_)drag({GET_X_LPARAM(b),GET_Y_LPARAM(b)});else if(resizing_){RECT r{};GetClientRect(window_,&r);ratio_=std::clamp(MulDiv(GET_X_LPARAM(b),1000,std::max(1L,r.right-MulDiv(5,dpi_,96))),200,800);layout();if(changed)changed();}return 0;
case WM_LBUTTONUP:if(drag_)drag({GET_X_LPARAM(b),GET_Y_LPARAM(b)});endDrag(true);resizing_=false;if(GetCapture()==window_)ReleaseCapture();return 0;
case WM_CANCELMODE:case WM_CAPTURECHANGED:endDrag();resizing_=false;if(GetCapture()==window_)ReleaseCapture();return 0;
case WM_SETCURSOR:{POINT point{};GetCursorPos(&point);ScreenToClient(window_,&point);if(PtInRect(&splitter_,point)){SetCursor(LoadCursorW(nullptr,IDC_SIZEWE));return TRUE;}break;}
case WM_MOUSEWHEEL:{POINT point{GET_X_LPARAM(b),GET_Y_LPARAM(b)};ScreenToClient(window_,&point);const int group=groupTab(1)<sessions_.size() && point.x>splitter_.left?1:0;offset_[group]=std::max(0,offset_[group]-GET_WHEEL_DELTA_WPARAM(a)/WHEEL_DELTA*MulDiv(90,dpi_,96));InvalidateRect(window_,nullptr,FALSE);return 0;}
case WM_CONTEXTMENU:{POINT point{GET_X_LPARAM(b),GET_Y_LPARAM(b)};if(point.x==-1&&point.y==-1){point={0,header_};ClientToScreen(window_,&point);menu(active_,point);return 0;}auto local=point;ScreenToClient(window_,&local);for(int group=0;group<2;++group)if(groupTab(group)<sessions_.size()){auto pane=groupBounds(group);RECT button{pane.right-MulDiv(34,dpi_,96),0,pane.right,header_};if(PtInRect(&button,local)){active_=groupTab(group);addMenu(point);return 0;}}const auto rects=bounds();for(size_t i=0;i<rects.size();++i)if(PtInRect(&rects[i],local)){menu(i,point);break;}return 0;}
case WM_NOTIFY:if(reinterpret_cast<NMHDR*>(b)->code==TTN_GETDISPINFOW){tooltipText_=description();reinterpret_cast<NMTTDISPINFOW*>(b)->lpszText=tooltipText_.data();}return 0;
}return DefWindowProcW(window_,m,a,b);}
}
