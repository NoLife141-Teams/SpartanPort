// SPDX-License-Identifier: GPL-2.0-or-later
// Black-box checks against physical file bytes, not text-mode stream positions.
#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>
#include "gpu/shadps4/video_core/renderer_vulkan/gow3_pair_capture.h"
using namespace Vulkan::Gow3;
using namespace Vulkan::Gow3::Pair;
namespace fs=std::filesystem;
static void Require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
static std::string Read(const fs::path& p) {
    std::ifstream f(p,std::ios::binary); Require(bool(f),"cannot read output");
    return {std::istreambuf_iterator<char>(f),{}};
}
static void Touch(const fs::path& p) { std::ofstream f(p,std::ios::binary); Require(bool(f),"cannot create trigger"); }
static uint64_t Field(const std::string& s,const std::string& key) {
    const std::string token='"'+key+"\":"; const auto pos=s.find(token);
    Require(pos!=std::string::npos,"missing numeric field"); return std::stoull(s.substr(pos+token.size()));
}
struct Observed { uint64_t retained; std::vector<uint64_t> draw_ends; };
static Observed Verify(const fs::path& p,uint64_t cap) {
    const auto s=Read(p); const auto end=s.rfind("{\"kind\":\"end\"");
    Require(end!=std::string::npos && s.back()=='\n',"missing complete footer");
    Require(Field(s.substr(end),"bytes_before_end")==end,"physical byte count differs from footer (Windows CR translation)");
    Require(s.size()==fs::file_size(p) && s.size()<=cap,"physical file exceeds byte budget");
    Require(s.find('\r')==std::string::npos,"unexpected translated CR in binary JSONL");
    Observed result{Field(s.substr(end),"retained_draws"),{}};
    for(size_t pos=0;(pos=s.find("{\"kind\":\"draw\"",pos))!=std::string::npos;) {
        pos=s.find('\n',pos); Require(pos!=std::string::npos,"partial draw line");
        result.draw_ends.push_back(++pos);
    }
    Require(result.retained==result.draw_ends.size(),"footer draw count differs");
    return result;
}
static void Draws(Capture& c,unsigned n) {
    for(unsigned i=0;i<n;++i) { c.Begin(12,34); c.Pending()->viewport_count=1; c.Recorded(); }
}
static Observed Run(const fs::path& root,const char* name,uint64_t cap) {
    const auto output=root/(std::string(name)+".jsonl"), trigger=root/(std::string(name)+".trigger"); Touch(trigger);
    Capture c(true,output,trigger,Capture::Capacity,false,false,cap);
    c.Boundary({1,1,0,false},1,Status::Missing); Draws(c,20);
    c.Boundary({2,1,0,true},2,Status::Missing); c.Boundary({3,1,0,false},3,Status::Missing);
    Require(c.GetStats().state==State::Done && !c.GetStats().errors,"capture failed");
    return Verify(output,cap);
}
int main(int argc,char** argv) try {
    Require(argc==2,"expected new test directory"); const auto root=fs::absolute(argv[1]);
    Require(!fs::exists(root),"test directory exists"); fs::create_directories(root);
    const auto pilot=Run(root,"pilot",65536); Require(pilot.draw_ends.size()==20,"pilot incomplete");
    // The budget line changes only by the number of decimal digits in its value.
    // Place the fourth draw exactly at the draw budget (footer reservation excluded).
    uint64_t exact=pilot.draw_ends[3]+Capture::FooterReserve;
    for(unsigned i=0;i<3;++i) exact=pilot.draw_ends[3]+Capture::FooterReserve+std::to_string(exact).size()-5;
    Require(exact>=8192 && std::to_string(exact).size()==std::to_string(exact-1).size(),"unexpected fixture size");
    const auto fits=Run(root,"fits",exact), short_one=Run(root,"short-one",exact-1);
    Require(fits.retained==4 && short_one.retained==3,"one-byte boundary did not reject exactly one draw");
    Require(fits.draw_ends.back()==exact-Capture::FooterReserve,"fourth draw not exactly at reserved boundary");
    Run(root,"minimum",8192);
    const auto old_path=root/"existing.jsonl", old_trigger=root/"existing.trigger"; Touch(old_trigger);
    const std::string sentinel("original\r\n\0\x1a",12);
    { std::ofstream f(old_path,std::ios::binary); f.write(sentinel.data(),sentinel.size()); }
    { Capture c(true,old_path,old_trigger); c.Boundary({1,1,0,false},1,Status::Missing);
      Require(c.GetStats().state==State::Failed,"existing output was accepted"); }
    Require(Read(old_path)==sentinel,"existing binary output changed");
    {
        Capture c(true,root/"group",root/"group-trigger",Capture::Capacity,true,true,8192); uint64_t id=1;
        for(const char* label:Capture::Labels) {
            const auto path=c.OutputPath(); Touch(c.TriggerPath()); c.Boundary({id++,1,0,false},1,Status::Missing);
            Draws(c,20); c.Boundary({id++,1,0,true},2,Status::Missing); c.Boundary({id++,1,0,false},3,Status::Missing);
            Verify(path,8192); (void)label;
        }
        Require(c.GetStats().state==State::Done,"group did not finish");
    }
    std::printf("Capture physical bytes: PASS (exact footer offsets; full/minimum budgets; exact-fit and one-byte-short draw reservation; four window resets; binary sentinel preserved)\n");
    return 0;
} catch(const std::exception& e) {
    std::fprintf(stderr,"Capture physical bytes: FAIL: %s\n",e.what()); return 1;
}
