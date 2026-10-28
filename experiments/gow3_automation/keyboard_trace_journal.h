#pragma once
#include <windows.h>
#include <cstdint>
#include <string_view>
namespace KeyTrace {
class Journal {
public:
    static constexpr uint64_t Maximum=65536,Reserve=2048;
    bool Open(const wchar_t* path,std::string_view header,uint64_t capacity=Maximum) {
        if(attempted || capacity>Maximum || capacity<Reserve || header.size()>capacity-Reserve) return false;
        attempted=true; cap=capacity;
        file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE) return false;
        if(GetFileType(file)!=FILE_TYPE_DISK || !Write(header)) {Close();return false;}
        return true;
    }
    bool Append(std::string_view line) {
        ++seen;
        if(failed || bytes>cap-Reserve || line.size()>cap-Reserve-bytes) {++dropped;return false;}
        if(!Write(line)) {++dropped;return false;}
        ++written; return true;
    }
    bool Finish(std::string_view footer) {
        bool ok=!failed && footer.size()<=Reserve && bytes<=cap && footer.size()<=cap-bytes && Write(footer);
        if(file!=INVALID_HANDLE_VALUE && !FlushFileBuffers(file)) ok=false;
        Close(); return ok;
    }
    void Close() {if(file!=INVALID_HANDLE_VALUE){CloseHandle(file);file=INVALID_HANDLE_VALUE;}}
    bool Failed() const {return failed;}
    uint64_t Bytes() const {return bytes;}
    uint64_t Seen() const {return seen;}
    uint64_t Written() const {return written;}
    uint64_t Dropped() const {return dropped;}
private:
    bool Write(std::string_view s) {
        DWORD done{};
        if(file==INVALID_HANDLE_VALUE) {failed=true;return false;}
        const BOOL ok=WriteFile(file,s.data(),static_cast<DWORD>(s.size()),&done,nullptr);
        bytes+=done;
        if(!ok || done!=s.size()) {failed=true;return false;}
        return true;
    }
    HANDLE file=INVALID_HANDLE_VALUE;
    bool attempted{},failed{};
    uint64_t cap{},bytes{},seen{},written{},dropped{};
};
}
