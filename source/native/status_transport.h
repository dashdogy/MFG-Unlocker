#pragma once
#include <Windows.h>
#include "diagnostic_paths.h"
#include <array>
#include <cstdint>
#include <cwchar>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>

namespace status_transport {
constexpr size_t kCapacity=1024*1024;
constexpr uint32_t kMagic=0x5354464d;
struct MemorySnapshot {
    uint32_t magic, pid;
    uint64_t birth;
    uint32_t bytes;
    char text[kCapacity];
};
inline std::wstring MemoryName(const std::wstring& primary) {
    if(primary.empty()||!diagnostic_paths::ProcessBirth())return {};
    wchar_t name[128]{};
    swprintf_s(name,L"Local\\RTXMFG-Status-%lu-%016llX-%016llX",GetCurrentProcessId(),
        static_cast<unsigned long long>(diagnostic_paths::ProcessBirth()),
        static_cast<unsigned long long>(diagnostic_paths::PathHash(primary)));
    return name;
}
struct MutexLock {
    HANDLE mutex=nullptr;
    bool held=false;
    // Readers (the menu, on the render thread) never wait; the writer waits out a reader's copy.
    explicit MutexLock(HANDLE value,DWORD wait=0):mutex(value) {
        if(mutex){const auto result=WaitForSingleObject(mutex,wait);held=result==WAIT_OBJECT_0||result==WAIT_ABANDONED;}
    }
    ~MutexLock(){if(held)ReleaseMutex(mutex);}
};
struct Writer {
    std::wstring path;
    HANDLE diskMutex=nullptr, memoryMutex=nullptr, mapping=nullptr;
    MemorySnapshot* view=nullptr;
    DWORD diskThread=0;
    uint64_t diskThreadBirth=0;
};
// Bounded process-lifetime handles, reclaimed by Windows. There is no disk
// fallback file to accumulate when a game holds its primary status file open.
inline std::array<Writer,16> writers{};
inline std::mutex writersMutex;
inline Writer* FindWriter(const std::wstring& primary) {
    for(auto& writer:writers)if(writer.path==primary)return &writer;
    for(auto& writer:writers)if(writer.path.empty()) {
        wchar_t name[96]{};
        swprintf_s(name,L"Local\\RTXMFG-StatusFile-%016llX",
            static_cast<unsigned long long>(diagnostic_paths::PathHash(primary)));
        writer.diskMutex=CreateMutexW(nullptr,FALSE,name);
        if(!writer.diskMutex)return nullptr;
        writer.path=primary;return &writer;
    }
    return nullptr;
}
inline bool OwnsDisk(Writer& writer) {
    FILETIME created{},exited{},kernel{},user{};
    if(!GetThreadTimes(GetCurrentThread(),&created,&exited,&kernel,&user))return false;
    const uint64_t birth=(uint64_t(created.dwHighDateTime)<<32)|created.dwLowDateTime;
    if(writer.diskThread==GetCurrentThreadId()&&writer.diskThreadBirth==birth)return true;
    const auto result=WaitForSingleObject(writer.diskMutex,0);
    if(result!=WAIT_OBJECT_0&&result!=WAIT_ABANDONED)return false;
    writer.diskThread=GetCurrentThreadId();
    writer.diskThreadBirth=birth;
    // Held until the backend writer thread exits. Another process cannot
    // replace its .tmp or primary file; mutex abandonment permits recovery.
    return true;
}
inline DWORD WriteMemory(Writer& writer,const std::string& text) {
    if(text.empty()||text.size()>kCapacity)return ERROR_INVALID_PARAMETER;
    const auto name=MemoryName(writer.path);
    if(name.empty())return ERROR_INVALID_NAME;
    if(!writer.memoryMutex)writer.memoryMutex=CreateMutexW(nullptr,FALSE,(name+L"-Lock").c_str());
    MutexLock lock(writer.memoryMutex,10);
    if(!lock.held)return ERROR_BUSY;
    bool created=false;
    if(!writer.mapping) {
        writer.mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(MemorySnapshot),name.c_str());
        if(!writer.mapping)return GetLastError();
        created=GetLastError()!=ERROR_ALREADY_EXISTS;
    }
    if(!writer.view)writer.view=static_cast<MemorySnapshot*>(MapViewOfFile(writer.mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(MemorySnapshot)));
    if(!writer.view){const auto error=GetLastError();CloseHandle(writer.mapping);writer.mapping=nullptr;return error;}
    auto& target=*writer.view;
    if(!created && (target.pid!=GetCurrentProcessId()||target.birth!=diagnostic_paths::ProcessBirth()))return ERROR_INVALID_DATA;
    target.magic=0;target.pid=GetCurrentProcessId();target.birth=diagnostic_paths::ProcessBirth();
    target.bytes=static_cast<uint32_t>(text.size());
    memcpy(target.text,text.data(),text.size());
    target.magic=kMagic;
    return ERROR_SUCCESS;
}
inline bool ReadMemory(const std::wstring& primary,std::string& output) {
    const auto name=MemoryName(primary);
    if(name.empty())return false;
    HANDLE mutex=OpenMutexW(SYNCHRONIZE|MUTEX_MODIFY_STATE,FALSE,(name+L"-Lock").c_str());
    if(!mutex)return false;
    bool okay=false;
    {
        MutexLock lock(mutex);
        if(lock.held) {
            HANDLE mapping=OpenFileMappingW(FILE_MAP_READ,FALSE,name.c_str());
            if(mapping) {
                const auto* view=static_cast<const MemorySnapshot*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,sizeof(MemorySnapshot)));
                if(view) {
                    if(view->magic==kMagic&&view->pid==GetCurrentProcessId()
                        &&view->birth==diagnostic_paths::ProcessBirth()&&view->bytes&&view->bytes<=kCapacity) {
                        output.assign(view->text,view->bytes);okay=true;
                    }
                    UnmapViewOfFile(view);
                }
                CloseHandle(mapping);
            }
        }
    }
    CloseHandle(mutex);return okay;
}
inline DWORD AtomicWrite(const std::wstring& path,const std::string& text) {
    if(path.empty()||text.empty()||text.size()>kCapacity)return ERROR_INVALID_PARAMETER;
    const auto temporary=path+L".tmp";
    HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return GetLastError();
    DWORD written=0;
    const BOOL wrote=WriteFile(file,text.data(),static_cast<DWORD>(text.size()),&written,nullptr);
    DWORD error=wrote?(written==text.size()?ERROR_SUCCESS:ERROR_WRITE_FAULT):GetLastError();
    if(!CloseHandle(file)&&!error)error=GetLastError();
    if(!error&&!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING))error=GetLastError();
    if(error)DeleteFileW(temporary.c_str());
    return error;
}
// fallbackError is the process-memory write's result; fallback means the file could not be replaced and the
// memory copy alone carries this publication. disk says whether the file write was attempted: disk=false skips
// it while the memory copy is written, and writes the file anyway when the memory copy fails.
struct Result {bool published=false;bool fallback=false;bool disk=false;DWORD primaryError=0;DWORD fallbackError=0;};
inline Result Publish(const std::wstring& primary,const std::string& text,bool disk=true) {
    Result result;
    if(primary.empty()||text.empty()||text.size()>kCapacity){result.primaryError=ERROR_INVALID_PARAMETER;return result;}
    std::lock_guard lock(writersMutex);
    auto* writer=FindWriter(primary);
    if(!writer){result.primaryError=ERROR_NOT_ENOUGH_MEMORY;return result;}
    // GitHub issue 122: every publication refreshes this process's memory copy first, so in-process readers
    // (the menu) read it instead of the file and the file can be written less often.
    result.fallbackError=WriteMemory(*writer,text);
    if(!disk&&result.fallbackError==ERROR_SUCCESS){result.published=true;return result;}
    result.disk=true;
    result.primaryError=OwnsDisk(*writer)?AtomicWrite(primary,text):ERROR_BUSY;
    if(!result.primaryError){result.published=true;return result;}
    if(result.primaryError!=ERROR_SHARING_VIOLATION&&result.primaryError!=ERROR_LOCK_VIOLATION
        &&result.primaryError!=ERROR_ACCESS_DENIED&&result.primaryError!=ERROR_BUSY)return result;
    result.published=result.fallbackError==ERROR_SUCCESS;
    result.fallback=result.published;
    return result;
}
// Source::Fallback is this process's memory copy. It is read first: every publication refreshes it before the
// file (a writer that cannot take its lock within 10 ms leaves it one publication behind). The file serves
// readers before the first publication or without the memory copy.
enum class Source {None,Primary,Fallback};
template<class Reader,class Validator>
Source Read(const std::wstring& primary,std::string& output,Reader reader,Validator valid) {
    std::string trial;
    if(ReadMemory(primary,trial)&&valid(trial)){output=std::move(trial);return Source::Fallback;}
    trial.clear();
    if(reader(primary,trial)&&valid(trial)){output=std::move(trial);return Source::Primary;}
    return Source::None;
}
}
