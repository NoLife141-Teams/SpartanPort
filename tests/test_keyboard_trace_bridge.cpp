#include "../src/keyboard_trace_bridge.h"
#include <windows.h>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
namespace fs=std::filesystem;
static std::string Bytes(const fs::path& p){std::ifstream f(p,std::ios::binary);assert(f);return {std::istreambuf_iterator<char>(f),{}};}
int main(int argc,char**argv){
    assert(argc==3);const std::string mode=argv[1];const fs::path dir=argv[2];assert(fs::is_directory(dir));
    const auto log=fs::absolute(dir/(mode+".jsonl"));const auto trigger=fs::absolute(dir/(mode+".trigger"));
    assert(!fs::exists(log)&&!fs::exists(trigger));
    _wputenv_s(L"SPARTAN_KEY_TRACE_FILE",log.c_str());_wputenv_s(L"SPARTAN_KEY_TRACE_TRIGGER",trigger.c_str());
    _putenv_s("SPARTAN_KEY_TRACE",mode=="off"?"":mode=="truthy"?"true":"1");
    if(mode=="existing"){std::ofstream f(log);f<<"preserve";}
    if(mode=="off"||mode=="truthy"||mode=="existing"){
        assert(!spartanport_key_trace_start());assert(!spartanport_key_trace_begin().id);
        spartanport_key_trace_window(7,KT_FOCUS,1);spartanport_key_trace_s(7,KT_FOCUS,1,0);spartanport_key_trace_tick();spartanport_key_trace_stop();
        if(mode=="existing")assert(Bytes(log)=="preserve");else assert(!fs::exists(log));
    }else{
        assert(spartanport_key_trace_start());spartanport_key_trace_window(7,KT_FOCUS,1);assert(!fs::exists(log));
        {std::ofstream f(trigger);f<<"start";}
        // Polling the trigger is deliberately limited to 10 Hz, without OS input.
        Sleep(110);spartanport_key_trace_window(7,KT_FOCUS,1);assert(fs::exists(log));
        if(mode=="expiry"){
            Sleep(10020);assert(!spartanport_key_trace_begin().id);
            spartanport_key_trace_tick();spartanport_key_trace_stop();
            const auto text=Bytes(log);assert(text.find("\"reason\":2")!=std::string::npos);
            assert(text.size()<=65536 && !spartanport_key_trace_enabled());
            puts("PASS bridge real-QPC expiry rejects producers at 10 seconds, before owner drain");return 0;
        }
        spartanport_key_trace_s(7,KT_FOCUS,1,0);auto t=spartanport_key_trace_begin();assert(t.id);
        t.flags=KT_S_VALID|KT_S_BEFORE|KT_S_AFTER|KT_HOST_Y_VALID|KT_FINAL_Y_VALID;t.host_y=t.final_y=255;spartanport_key_trace_end(t);
        spartanport_key_trace_s(7,KT_FOCUS,0,0);t=spartanport_key_trace_begin();t.flags=KT_S_VALID|KT_HOST_Y_VALID|KT_FINAL_Y_VALID;t.host_y=t.final_y=128;spartanport_key_trace_end(t);
        spartanport_key_trace_window(7,0,0);spartanport_key_trace_s(7,0,1,0);
        spartanport_key_trace_stop();const auto once=Bytes(log);
        spartanport_key_trace_window(7,KT_FOCUS,1);spartanport_key_trace_stop();assert(Bytes(log)==once);
        assert(!spartanport_key_trace_begin().id && once.find("\"type\":\"stop\"")!=std::string::npos && once.size()<=65536);
        assert(once.find("text_recorded\":false")!=std::string::npos);
        assert(once.find("\"keys\":[\"S\"]")!=std::string::npos && once.find("\"axes\":[\"left_y\"]")!=std::string::npos);
        assert(once.find("\"final_y\":255")!=std::string::npos && once.find("\"final_y\":128")!=std::string::npos);
        assert(once.find("\"type\":\"k\"")==std::string::npos);
    }
    printf("PASS bridge %s, no SDL window or host input\n",mode.c_str());
}
