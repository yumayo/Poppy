#include <winsock2.h>
#include <ws2tcpip.h>
#include "hook_server.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>

using namespace poppy;
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Socket {
    SOCKET value=INVALID_SOCKET;
    ~Socket(){if(value!=INVALID_SOCKET)closesocket(value);}
};
void connect(Socket& socket,unsigned short port){
    socket.value=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);check(socket.value!=INVALID_SOCKET,"Cannot create test socket");
    DWORD timeout=3000;setsockopt(socket.value,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(port);
    check(::connect(socket.value,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"Cannot reach hook listener");
}
int request(unsigned short port,const std::string& path,const std::string& body,const char* method="POST",const std::string& extra=""){
    Socket socket;connect(socket,port);
    const auto text=std::string(method)+" "+path+" HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: "+std::to_string(body.size())+"\r\n"+extra+"\r\n"+body;
    size_t offset=0;while(offset<text.size()){const auto sent=::send(socket.value,text.data()+offset,static_cast<int>(text.size()-offset),0);check(sent>0,"Cannot send hook request");offset+=sent;}
    char response[256]{};check(recv(socket.value,response,sizeof(response)-1,0)>0,"Hook request timed out");
    return std::stoi(std::string(response).substr(9,3));
}
void tests(){
    _putenv_s("POPPY_HOOK_BIND","127.0.0.1");_putenv_s("POPPY_HOOK_PORT","0");_putenv_s("POPPY_HOOK_HOST","127.0.0.1");
    HWND window=CreateWindowExW(0,L"STATIC",L"Hook test",0,0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);
    check(window!=nullptr,"Cannot create completion window");
    HookServer server;check(server.start(window),"Cannot start hook listener");
    const auto port=static_cast<unsigned short>(std::stoul(server.url().substr(server.url().rfind(':')+1)));
    const auto a=newSessionId(),b=newSessionId();check(a!=b&&validNotificationId(a)&&validNotificationId(b),"Terminal starts share notification IDs");
    const auto pathA="/tasks/sessions/"+a+"/status",pathB="/tasks/sessions/"+b+"/status";
    check(request(port,pathA,R"({"status":"running"})")==404,"Unregistered hook accepted");
    server.add(a);server.add(b);
    Socket slow;connect(slow,port);const char partial[]="POST /tasks/ HTTP/1.1\r\n";::send(slow.value,partial,sizeof(partial)-1,0);
    check(request(port,pathB,R"({"status":"waiting"})")==204,"A slow sender blocked another terminal");
    check(request(port,pathA,R"({"status":"running"})")==204,"Valid status failed");
    const auto events=server.drain();check(events.size()==2&&events[0].sessionId==b&&events[1].sessionId==a,"Events were routed to the active terminal instead of their IDs");
    check(request(port,"/tasks/sessions/"+a+"/activity",R"({"activityText":"日本語"})","PUT")==204,"Activity request failed");
    const auto activity=server.drain();check(activity.size()==1&&!activity[0].state&&activity[0].activity=="日本語","Activity altered state or lost Unicode");
    check(request(port,pathA,R"({"status":"bad"})")==400,"Unknown status accepted");
    check(request(port,pathA,std::string(8193,'a'))==413,"Oversized request accepted");
    check(request(port,pathA,R"({"status":"running"})","POST","Content-Length: 20\r\n")==400,"Duplicate length accepted");
    check(request(port,pathA,R"({"status":"completed"})")==204,"Completion failed");
    server.remove(a);check(server.drain().empty(),"Closed terminal retained queued events");
    check(request(port,pathA,R"({"status":"running"})")==404,"Late notification for a closed terminal accepted");
    check(request(port,pathB,R"({"status":"running"})")==204,"Closing another terminal removed this registration");
    server.stop();DestroyWindow(window);
}
}
int main(){try{tests();std::cout<<"Hook server tests passed\n";}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
