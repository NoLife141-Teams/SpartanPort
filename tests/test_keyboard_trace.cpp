// CPU-only: no SDL window, host input, game, save or guest memory.
#include "../experiments/gow3_automation/keyboard_trace_core.h"
#include "../experiments/gow3_automation/keyboard_trace_journal.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
using namespace KeyTrace;
namespace fs=std::filesystem;
static std::vector<Record> DrainAll(Core& c) {
    std::array<Record,Core::Capacity> data;bool busy=false;
    const auto n=c.Drain(data,busy);assert(!busy);return {data.begin(),data.begin()+n};
}
static std::string Bytes(const fs::path& p) {
    std::ifstream f(p,std::ios::binary);assert(f);return {std::istreambuf_iterator<char>(f),{}};
}
int main(int argc,char** argv) {
    assert(argc==2);const fs::path dir=argv[1];assert(fs::is_directory(dir));
    {
        Core c;assert(!c.Begin(50).id);c.End({},60);c.Key(60,1,KT_FOCUS,1,true,false);
        assert(c.Counts().offers==0 && DrainAll(c).empty());
        puts("PASS disabled core emits nothing");
    }
    {
        Core c;assert(c.Start(100,1000,7,KT_FOCUS));assert(c.Deadline()==10100);
        assert(!c.Start(200,1000,7,KT_FOCUS));
        auto t=c.Begin(10099);assert(t.id);c.End(t,10099);
        assert(!c.Begin(10100).id && !c.Active() && c.StopReason()==Expired);
        for(const auto& r:DrainAll(c))assert(r.end<10100);
        puts("PASS strict 10-second boundary and no reactivation");
    }
    {
        Core c;assert(c.Start(100,1000,7,KT_FOCUS));
        c.Key(110,7,KT_FOCUS,2,true,false);
        auto t=c.Begin(111);t.flags=KT_S_VALID|KT_S_BEFORE|KT_S_AFTER|KT_HOST_Y_VALID|KT_FINAL_Y_VALID;
        t.host_y=t.final_y=255;c.End(t,112);
        c.Key(120,7,KT_FOCUS,3,false,false);
        t=c.Begin(121);t.flags=KT_S_VALID|KT_HOST_Y_VALID|KT_FINAL_Y_VALID;t.host_y=t.final_y=128;c.End(t,122);
        auto rows=DrainAll(c);assert(rows.size()==5);
        assert(rows[1].kind==S && rows[1].flags==1 && rows[3].kind==S && rows[3].flags==0);
        assert(rows[2].final_y==255 && rows[4].final_y==128);
        assert(rows[2].pad_y==0 && !(rows[4].flags&KT_S_AFTER));
        assert(rows[2].first!=rows[4].first);
        puts("PASS press and release keep distinct event/read identities");
    }
    {
        Core c;assert(c.Start(100,1000,7,KT_FOCUS));auto t=c.Begin(110);
        c.Observe(111,0,2);t.flags=2047;t.pad_y=180;t.host_y=t.final_y=255;c.End(t,112);
        c.Key(113,7,0,2,true,false); // excluded while not focused
        c.Key(114,8,KT_FOCUS,2,true,false); // wrong window
        c.Observe(120,KT_FOCUS|KT_CAPTURE,3);
        c.Key(121,7,KT_FOCUS|KT_CAPTURE,3,true,false);
        c.Observe(125,KT_FOCUS|KT_TEXT,4);c.Key(126,7,KT_FOCUS|KT_TEXT,4,true,false);
        c.Observe(130,KT_FOCUS,5);auto u=c.Begin(131);u.flags=KT_S_VALID;c.End(u,132);
        const auto rows=DrainAll(c);unsigned keys=0,redacted=0;
        for(const auto& r:rows){keys+=r.kind==S;if(r.kind==Read && r.first==t.id){assert((r.flags&~(KT_HOST_BLOCK|KT_GATE_BLOCK|KT_HOLD_BEFORE|KT_HOLD_AFTER))==0);assert(!r.pad_y&&!r.host_y&&!r.final_y);++redacted;}}
        assert(keys==0 && redacted==1 && c.Counts().redacted==1);
        puts("PASS focus loss/capture/text/wrong window suppress keys; crossing read is redacted");
    }
    {
        Core c;assert(c.Start(100,1000,7,KT_FOCUS));
        for(uint64_t i=0;i<20;++i){auto t=c.Begin(110+i*2);t.flags=KT_S_VALID;c.End(t,111+i*2);}
        auto rows=DrainAll(c);assert(rows.size()==2 && rows[1].count==20 && rows[1].last-rows[1].first==19 && rows[1].max_gap==1);
        assert(c.Counts().coalesced==19);
        puts("PASS identical reads coalesce with explicit IDs, count and maximum gap");
    }
    {
        Core c;assert(c.Start(100,1000,7,KT_FOCUS));
        for(unsigned i=0;i<140;++i)c.Key(110+i,7,KT_FOCUS,i,(i&1)!=0,false);
        assert(c.StopReason()==Saturated && c.Counts().full==1 && DrainAll(c).size()==Core::Capacity);
        puts("PASS bounded queue saturation stops capture and counts the loss");
    }
    {
        Core c;assert(c.Start(100,1000,7,KT_FOCUS));auto t=c.Begin(200);c.End(t,199);
        assert(c.StopReason()==ClockFault && !c.Active());
        puts("PASS regressing read clock stops capture");
    }
    {
        Core c;assert(c.Start(100,1000000000,7,KT_FOCUS));
        std::atomic<uint64_t> ticks{110};std::atomic<unsigned> running{4};std::vector<std::thread> threads;
        for(unsigned j=0;j<4;++j)threads.emplace_back([&]{for(unsigned i=0;i<1000;++i){auto t=c.Begin(ticks.fetch_add(1));t.flags=KT_S_VALID;c.End(t,ticks.fetch_add(1));}--running;});
        uint64_t retained=0;
        while(running.load()){
            std::array<Record,Core::Capacity> rows;bool busy=false;const auto n=c.Drain(rows,busy);
            for(size_t i=0;i<n;++i){assert(rows[i].end>=rows[i].begin);if(rows[i].kind==Read){assert(rows[i].last-rows[i].first+1==rows[i].count);retained+=rows[i].count;}}
            std::this_thread::yield();
        }
        for(auto& t:threads)t.join();c.RequestStop(Closed);
        for(const auto& r:DrainAll(c))if(r.kind==Read)retained+=r.count;
        const auto n=c.Counts();assert(retained<=n.reads && n.queued+n.coalesced+n.busy+n.full<=n.offers);
        printf("PASS concurrent producers: reads=%llu retained=%llu busy=%llu full=%llu\n",(unsigned long long)n.reads,(unsigned long long)retained,(unsigned long long)n.busy,(unsigned long long)n.full);
    }
    {
        const auto path=dir/"exact-limit.jsonl";assert(!fs::exists(path));Journal j;
        const std::string header="H\n",row="R\n",footer(Journal::Reserve,'F');
        const auto cap=Journal::Reserve+header.size()+row.size();
        assert(j.Open(path.c_str(),header,cap));assert(j.Append(row));assert(!j.Append("X"));
        assert(j.Dropped()==1 && j.Finish(footer));assert(j.Bytes()==cap && fs::file_size(path)==cap);
        Journal again;assert(!again.Open(path.c_str(),"overwrite\n"));assert(Bytes(path)==header+row+footer);
        puts("PASS exact Windows byte bound, reserved footer and refusal to overwrite");
    }
    {
        Core c;assert(c.Start(100,1000,7,KT_FOCUS));
        for(unsigned i=0;i<3;++i){auto t=c.Begin(110+2*i);t.flags=KT_S_VALID|KT_HOST_Y_VALID|KT_FINAL_Y_VALID;
            t.host_y=t.final_y=i==1?255:128;t.pad_y=250;c.End(t,111+2*i);}
        auto rows=DrainAll(c);assert(rows.size()==4);
        assert(rows[1].final_y==128 && rows[2].final_y==255 && rows[3].final_y==128);
        for(const auto& r:rows)assert(!r.pad_y); // Missing validity is unknown, never a value.
        puts("PASS changing Y values cannot coalesce; invalid axis payload is discarded");
    }
    {
        Core c;assert(c.Start(100,1000,7,KT_FOCUS));
        c.Key(110,7,KT_FOCUS,4,true,false);c.Key(111,7,KT_FOCUS,4,false,false);
        auto t=c.Begin(112);t.flags=KT_S_VALID|KT_HOST_Y_VALID|KT_FINAL_Y_VALID;t.host_y=t.final_y=128;c.End(t,113);
        const auto rows=DrainAll(c);assert(rows.size()==4 && rows[1].pump==rows[2].pump);
        assert(rows[1].flags==1 && rows[2].flags==0 && rows[3].final_y==128 && !(rows[3].flags&(KT_S_BEFORE|KT_S_AFTER)));
        puts("PASS same-pump S pulse and later neutral sample remain distinct observations");
    }
    puts("11 core/journal scenarios passed");
}
