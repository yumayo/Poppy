#include "ai_status.h"
#include <iostream>
#include <stdexcept>

using namespace poppy;
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
const std::string id="0123456789abcdef0123456789abcdef";
AiEvent event(const char* body,bool activity=false){const auto value=parseAiEvent(id,activity,body);check(value.has_value(),"Valid hook rejected");return *value;}
void transitions(){
    AiStatus status;
    check(!status.apply(event(R"({"status":"running","onlyIfActive":true})"),1,false),"Conditional start activated an idle shell");
    status.apply(event(R"({"status":"running"})"),2,false);
    check(status.state==AiState::Running && status.detected && !status.unread && status.source==AiSource::Hook && status.confidence==AiConfidence::Confirmed,"Start did not establish confirmed hook status");
    status.apply(event(R"({"status":"waiting","activityText":"choose"})"),3,false);
    check(status.state==AiState::Waiting && status.unread,"Hidden waiting notification was lost");
    status.acknowledge();check(status.state==AiState::Waiting && !status.unread,"Acknowledging changed the AI state");
    status.apply(event(R"({"status":"running","onlyIfActive":true})"),4,true);
    status.apply(event(R"({"status":"completed","activityText":"done"})"),5,false);
    check(status.state==AiState::Completed && status.unread,"Completion did not notify");
    check(!status.apply(event(R"({"status":"running","onlyIfActive":true})"),6,false),"Late conditional resume overwrote completion");
    check(!status.apply(event(R"({"activityText":"late"})",true),7,false) && status.activity=="done","Late activity overwrote completion");
    status.acknowledge();check(status.state==AiState::Completed,"Selecting a terminal cleared completed state");
    status.apply(event(R"({"status":"running"})"),8,false);
    status.apply(event(R"({"status":"interrupted","onlyIfActive":true})"),9,false);
    check(status.state==AiState::Paused,"Interrupt was not preserved");
    check(!status.apply(event(R"({"status":"running","onlyIfActive":true})"),10,false),"Late resume overwrote interruption");
    status.apply(event(R"({"status":"none"})"),11,false);
    check(status.state==AiState::Idle && !status.unread && !status.detected && status.activity.empty(),"none did not clear the status and activity");
    status.apply(event(R"({"status":"running"})"),12,true);
    status.apply(event(R"({"activityText":"completed? approval?"})",true),13,true);
    check(status.state==AiState::Running && status.activity=="completed? approval?","Activity text inferred a state");
    check(status.changedAt==12,"Activity changed the transition timestamp");
}
void aggregation(){
    AiStatus complete,running,waiting,paused,shell;
    complete.apply(event(R"({"status":"completed"})"),1,false);
    running.apply(event(R"({"status":"running"})"),2,false);
    waiting.apply(event(R"({"status":"waiting"})"),3,false);
    paused.apply(event(R"({"status":"interrupted"})"),4,false);
    auto result=summarizeAi({complete,running,waiting,paused,shell});
    check(result.state==AiState::Waiting && result.unread && result.counts[static_cast<size_t>(AiState::Running)]==1 && result.counts[static_cast<size_t>(AiState::Idle)]==0,"Mixed AI/shell aggregation is incorrect");
    check(summarizeAi({complete,running,paused}).state==AiState::Running,"Completed/interrupted masked a running AI");
    check(summarizeAi({complete,paused}).state==AiState::Paused,"Completion masked interruption");
}
void validation(){
    for(const auto body:{"{}","[]",R"({"status":"failed"})",R"({"status":"running","status":"completed"})",R"({"status":"running","onlyIfActive":"true"})",R"({"status":"waiting","activityText":3})",R"({"status":"running"}garbage)",R"({"status":"running","activityText":"\ud800"})",R"({"status":"running","activityText":"\uDC00"})",R"({"status":"running","x":{}})"})check(!parseAiEvent(id,false,body),"Malformed/unsupported hook accepted");
    check(!parseAiEvent("ABC",false,R"({"status":"running"})"),"Unregistered ID syntax accepted");
    check(!parseAiEvent(id,true,R"({"status":"running","activityText":"bad"})"),"Activity endpoint accepted status");
    auto value=event(R"({"status":"waiting","activityText":"日本語 \ud83d\ude00\nline"})");
    check(value.activity=="日本語 😀\nline","Unicode JSON was decoded incorrectly");
    check(!parseAiEvent(id,false,"{\"status\":\"running\",\"activityText\":"+jsonString(std::string(513,'a'))+"}"),"Oversized activity accepted");
    check(!parseAiEvent(id,false,std::string(8193,' ')),"Oversized request accepted");
    check(!parseAiEvent(id,false,"{\"status\":\"running\",\"activityText\":\"\xc0\x80\"}"),"Invalid UTF-8 accepted");
    check(parseAiEvent(id,false,"{\"status\":\"running\",\"activityText\":"+jsonString("quotes \" slash \\ newline\n")+"}").has_value(),"Escaped JSON was rejected");
}
}
int main(){try{transitions();aggregation();validation();std::cout<<"AI status tests passed\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
