#include "status_transport.h"
#include "ui_status_json.h"
#include <cstdio>
#include <filesystem>
#include <thread>
static bool okay=true;
static void Check(bool value,const char* name){printf("%s %s\n",value?"PASS":"FAIL",name);fflush(stdout);okay&=value;}
static bool Read(const std::wstring& path,std::string& text){
    HANDLE f=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    if(f==INVALID_HANDLE_VALUE)return false;
    DWORD size=GetFileSize(f,nullptr),read=0;
    if(!size||size>1024*1024){CloseHandle(f);return false;}
    text.resize(size);const bool okay=ReadFile(f,text.data(),size,&read,nullptr)&&read==size;CloseHandle(f);return okay;
}
int main(){
    using namespace status_transport;
    const auto primary=(std::filesystem::current_path()/L"transport.status.json").wstring();
    const auto fallback=MemoryName(primary);
    Check(!fallback.empty()&&fallback==MemoryName(primary),"memory channel identity is stable within the process lifetime");
    Check(fallback!=MemoryName(primary+L".other"),"different configured primary paths do not share a memory channel");
    auto first=Publish(primary,"{\"epoch\":1}");
    Check(first.published&&!first.fallback&&!first.primaryError,"normal publication retains the configured primary path");
    HANDLE lock=CreateFileW(primary.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    Check(lock!=INVALID_HANDLE_VALUE,"GTA-shaped read handle denies write and delete sharing");if(lock==INVALID_HANDLE_VALUE)return 1;
    auto second=Publish(primary,"{\"epoch\":2}");
    // MoveFileEx may report ACCESS_DENIED for a target whose open handle denies
    // deletion, even when a direct DELETE-access probe reports SHARING_VIOLATION.
    printf("NOTE blocked MoveFileEx error=%lu\n",second.primaryError);
    Check(second.published&&second.fallback&&(second.primaryError==ERROR_SHARING_VIOLATION
        ||second.primaryError==ERROR_ACCESS_DENIED)&&!second.fallbackError,"blocked atomic replace publishes complete process-memory fallback");
    std::string text;
    Check(Read(primary,text)&&text=="{\"epoch\":1}","locked primary bytes remain intact");
    auto current=[](const std::string& value){return ui_status_json::CompleteObject(value)&&value=="{\"epoch\":2}";};
    Check(status_transport::Read(primary,text,&::Read,current)==Source::Fallback&&current(text),"reader rejects old primary and selects validated current fallback");
    Publish(primary,"{\"epoch\":2");text="unchanged";
    Check(status_transport::Read(primary,text,&::Read,current)==Source::None&&text=="unchanged","truncated fallback is rejected without exposing partial output");
    Publish(primary,"{\"epoch\":0}");
    Check(status_transport::Read(primary,text,&::Read,current)==Source::None,"stale fallback cannot bypass the caller validity policy");
    size_t count=0;
    for(const auto& entry:std::filesystem::directory_iterator(std::filesystem::current_path()))
        if(entry.path().filename().wstring().find(L".status.json")!=std::wstring::npos)++count;
    Check(count==1,"locked-file fallback creates no extra status or temporary files");
    Check(CloseHandle(lock)!=FALSE,"foreign file ownership is released by its owner");
    auto recovery=Publish(primary,"{\"epoch\":2}");
    Check(recovery.published&&!recovery.fallback&&!recovery.primaryError,"publication returns to primary when the game releases its handle");
    Check(Read(primary,text)&&text=="{\"epoch\":2}","recovered primary file holds the current snapshot");
    Check(status_transport::Read(primary,text,&::Read,current)==Source::Fallback&&current(text),"reader takes the current snapshot from process memory, which every publication refreshes");
    // GitHub issue 122: a memory-only publication leaves the file alone and still reaches in-process readers.
    auto quiet=Publish(primary,"{\"epoch\":3}",false);
    Check(quiet.published&&!quiet.disk&&!quiet.fallback&&!quiet.primaryError&&!quiet.fallbackError,"memory-only publication skips the file");
    Check(Read(primary,text)&&text=="{\"epoch\":2}","memory-only publication leaves the file's bytes unchanged");
    auto third=[](const std::string& value){return ui_status_json::CompleteObject(value)&&value=="{\"epoch\":3}";};
    Check(status_transport::Read(primary,text,&::Read,third)==Source::Fallback&&third(text),"reader sees the memory-only publication");
    auto loud=Publish(primary,"{\"epoch\":4}");
    Check(loud.published&&loud.disk&&!loud.fallback&&!loud.primaryError&&Read(primary,text)&&text=="{\"epoch\":4}","the next file publication writes the current snapshot");
    auto fourth=[](const std::string& value){return ui_status_json::CompleteObject(value)&&value=="{\"epoch\":4}";};
    Check(status_transport::Read(primary,text,&::Read,fourth)==Source::Fallback,"memory and file agree after a file publication");
    // A memory copy the writer cannot update (a reader holds its lock past the writer's 10 ms) sends a
    // memory-only publication to the file, so readers never lose the current snapshot.
    const auto lockName=MemoryName(primary)+L"-Lock";
    HANDLE held=CreateEventW(nullptr,TRUE,FALSE,nullptr),done=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    bool holding=false;
    std::thread holder([&]{
        HANDLE mutex=OpenMutexW(SYNCHRONIZE|MUTEX_MODIFY_STATE,FALSE,lockName.c_str());
        holding=mutex&&WaitForSingleObject(mutex,1000)==WAIT_OBJECT_0;
        SetEvent(held);
        if(holding){WaitForSingleObject(done,5000);ReleaseMutex(mutex);}
        if(mutex)CloseHandle(mutex);
    });
    WaitForSingleObject(held,5000);
    auto busy=Publish(primary,"{\"epoch\":5}",false);
    SetEvent(done);holder.join();CloseHandle(held);CloseHandle(done);
    Check(holding,"test thread holds the memory lock");
    Check(busy.published&&busy.disk&&busy.fallbackError==ERROR_BUSY&&!busy.primaryError&&Read(primary,text)&&text=="{\"epoch\":5}",
        "a memory copy the writer cannot update sends the memory-only publication to the file");
    auto fifth=[](const std::string& value){return ui_status_json::CompleteObject(value)&&value=="{\"epoch\":5}";};
    Check(status_transport::Read(primary,text,&::Read,fifth)==Source::Primary&&fifth(text),"reader falls back to the file when the memory copy is behind");
    auto missing=Publish((std::filesystem::current_path()/L"does-not-exist"/L"status.json").wstring(),"{}");
    Check(!missing.published&&!missing.fallback&&missing.primaryError==ERROR_PATH_NOT_FOUND,"unrelated path errors are not silently redirected");
    printf("COMPLETE status transport passed=%u\n",okay?1:0);return okay?0:1;
}
