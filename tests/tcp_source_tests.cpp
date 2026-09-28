#include "tcp_source.hpp"
#include <future>
#include <iostream>
using namespace wave;
void require(bool ok) {if(!ok) throw std::runtime_error("TCP integration assertion failed");}
void receiver_time_test() {
    DataStore store(1,32,{}); tcp::Receiver receiver(store,0); std::atomic<bool> stop{false};
    auto work=std::async(std::launch::async,[&]{
        unsigned id=1;
        auto connect_client=[&](tcp::Socket& s) {
            s.value=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP); sockaddr_in a{};
            a.sin_family=AF_INET; a.sin_port=htons(receiver.listen_port()); a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
            require(connect(s.value,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0);
        };
        auto register_client=[&](SOCKET s) {
            tcp::Writer hello; hello.text("timed"); hello.text("Timed source");
            tcp::request(s,tcp::Hello,id++,hello,stop);
            tcp::Writer schema; schema.u32(1); schema.text("v"); schema.text("Value"); schema.text("");
            schema.u32(1); schema.u32(2); schema.f64(0);
            return tcp::request(s,tcp::Register,id++,schema,stop);
        };
        {
            tcp::Socket s; connect_client(s); register_client(s.value);
            tcp::Writer sample; sample.f32(12);
            auto first=tcp::request(s.value,tcp::DataNow,id++,sample,stop);
            tcp::Reader a{first.payload}; require(a.u64()==1 && a.f64()==0); a.end();
            std::this_thread::sleep_for(std::chrono::milliseconds(70));
            auto second=tcp::request(s.value,tcp::DataNow,id++,sample,stop);
            tcp::Reader b{second.payload}; require(b.u64()==2 && b.f64()>=0.06); b.end();
            bool rejected=false;
            try {tcp::request(s.value,tcp::DataNow,id++,{},stop);} catch(const std::exception&) {rejected=true;}
            require(rejected);
            rejected=false;
            tcp::Writer batch; batch.u64(2); batch.u32(1); batch.f32(3);
            try {tcp::request(s.value,tcp::Data,id++,batch,stop);} catch(const std::exception&) {rejected=true;}
            require(rejected);
        }
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(receiver.connected && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        require(!receiver.connected);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        {
            tcp::Socket s; connect_client(s); auto reply=register_client(s.value);
            tcp::Reader r{reply.payload}; require(r.u64()==2);
            tcp::Writer sample; sample.f32(42);
            auto third=tcp::request(s.value,tcp::DataNow,id++,sample,stop);
            tcp::Reader c{third.payload}; require(c.u64()==3 && c.f64()>=0.14);
        }
    });
    while(work.wait_for(std::chrono::milliseconds(1))!=std::future_status::ready) receiver.poll();
    work.get(); receiver.stop();
    const auto points=store.query(0,0,100,100);
    require(store.reception_time() && points.size()==3 && points[0].t==0 && points[2].value==42 && points[2].t>points[1].t);
    require(!receiver.connected && store.channels().size()==1);
    std::cout<<"PASS receiver-time protocol: assigned timestamps, invalid payload/mode, disconnect and reconnect continuity\n";
}
void multiple_sources_test() {
    SignalHub hub; tcp::Receiver receiver(hub,0); std::atomic<bool> stop{false};
    auto work=std::async(std::launch::async,[&]{
        unsigned id=1;
        auto connect_client=[&](tcp::Socket& s) {
            s.value=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP); sockaddr_in a{};
            a.sin_family=AF_INET; a.sin_port=htons(receiver.listen_port()); a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
            require(connect(s.value,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0);
        };
        auto hello=[&](SOCKET socket,const char* name) {
            tcp::Writer w; w.text(name); w.text(name); return tcp::request(socket,tcp::Hello,id++,w,stop);
        };
        auto schema=[&](SOCKET socket,unsigned mode,double rate) {
            tcp::Writer w; w.u32(1); w.text("same-id"); w.text("Value"); w.text(""); w.u32(1); w.u32(mode); w.f64(rate);
            return tcp::request(socket,tcp::Register,id++,w,stop);
        };
        tcp::Socket slow, audio, stalled, duplicate;
        connect_client(slow); hello(slow.value,"telemetry"); schema(slow.value,2,0);
        tcp::Writer single; single.f32(11); tcp::request(slow.value,tcp::DataNow,id++,single,stop);
        connect_client(stalled); char byte=0; require(tcp::transfer(stalled.value,&byte,1,true,stop));
        // A partial header on another peer must not hold up either valid stream.
        connect_client(audio); hello(audio.value,"audio"); schema(audio.value,1,48000);
        connect_client(duplicate); bool rejected=false;
        try {hello(duplicate.value,"telemetry");} catch(const std::exception&) {rejected=true;}
        require(rejected);
        for(unsigned packet=0;packet<4;++packet) {
            tcp::Writer batch; batch.u64(packet*1920); batch.u32(1920);
            for(unsigned i=0;i<1920;++i) batch.f32(float(i%11));
            tcp::request(audio.value,tcp::Data,id++,batch,stop);
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
        tcp::request(slow.value,tcp::DataNow,id++,single,stop);
        tcp::request(slow.value,tcp::ClearSource,id++,{},stop);
        closesocket(slow.value); slow.value=INVALID_SOCKET;
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        tcp::Socket reconnect; connect_client(reconnect); hello(reconnect.value,"telemetry");
        auto ack=schema(reconnect.value,2,0); tcp::Reader r{ack.payload}; require(r.u64()==2);
        tcp::request(reconnect.value,tcp::DataNow,id++,single,stop);
        // The still-connected audio peer continues independently.
        tcp::Writer last; last.u64(7680); last.u32(1); last.f32(99);
        tcp::request(audio.value,tcp::Data,id++,last,stop);
    });
    while(work.wait_for(std::chrono::milliseconds(1))!=std::future_status::ready) receiver.poll();
    work.get(); receiver.stop();
    require(hub.channels().size()==2 && hub.label(0)=="Value (1)" && hub.label(1)=="Value (2)");
    auto slow=hub.query(0,-100,100,100), audio=hub.query(1,-100,100,10000);
    require(slow.size()==1 && audio.size()==7681 && audio.back().value==99);
    require(audio.front().t>-0.05 && audio.front().t<0.2 && slow[0].t>0.2);
    const auto frozen=hub.snapshot(-100,100);
    hub.set_capacity_frames(8);
    require(hub.find("telemetry")->data->capacity_frames()==8 && hub.find("audio")->data->capacity_frames()==8);
    require(hub.query(1,-100,100,100).size()==8 && frozen.query(1,-100,100,10000).size()==7681);
    hub.clear(); require(hub.range().empty && !frozen.range().empty);
    std::cout<<"PASS concurrent mixed-rate/mode sources, same signal IDs, blocked peer isolation, ownership, clear scope, reconnect and snapshots\n";
}
int main() {
    try {
        multiple_sources_test();
        receiver_time_test();
        DataStore store(1,600,{}); tcp::Receiver receiver(store,0); std::atomic<bool> stop{false};
        auto connect_client=[&](tcp::Socket& s) {s.value=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP); sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons(receiver.listen_port()); a.sin_addr.s_addr=htonl(INADDR_LOOPBACK); require(connect(s.value,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0);};
        unsigned id=1;
        auto call=[&](SOCKET s,unsigned type,const tcp::Writer& w,bool success=true,bool split=false) {
            const auto request_id=id++;
            auto work=std::async(std::launch::async,[&]{
                auto wire=tcp::encode({type,request_id,w.bytes});
                if(split) {for(char& c:wire) require(tcp::transfer(s,&c,1,true,stop));}
                else require(tcp::transfer(s,wire.data(),wire.size(),true,stop));
                tcp::Message reply; require(tcp::read_message(s,reply,stop,false)); return reply;
            });
            while(work.wait_for(std::chrono::milliseconds(1))!=std::future_status::ready) receiver.poll();
            auto reply=work.get(); require(reply.type==tcp::Reply && reply.request==request_id);
            tcp::Reader r{reply.payload}; require((r.u32()==0)==success); const auto error=r.text(); require(success || !error.empty());
            reply.payload.erase(reply.payload.begin(),reply.payload.begin()+r.pos); return reply;
        };
        auto hello=[](std::string source="test") {tcp::Writer w; w.text(source); w.text("Test source"); return w;};
        auto schema=[](bool duplicate=false,double rate=25) {tcp::Writer w; w.u32(2);
            for(int i=0;i<2;++i){w.text(i && !duplicate?"b":"a"); w.text(i?"Temperature":"Current"); w.text(i?"C":"A"); w.u32(1); w.u32(1); w.f64(rate);} return w;};
        auto samples=[](std::uint64_t first,std::initializer_list<float> values){tcp::Writer w; w.u64(first); w.u32(unsigned(values.size()/2)); for(float v:values) w.f32(v); return w;};
        require(store.channels().empty());
        {
            tcp::Socket s; connect_client(s);
            call(s.value,tcp::Data,samples(0,{1,2}),false);
            call(s.value,tcp::Register,schema(),false);
            call(s.value,tcp::Hello,hello(),true,true);
            call(s.value,tcp::Register,schema(true),false); require(store.channels().empty());
            auto ack=call(s.value,tcp::Register,schema()); tcp::Reader r{ack.payload}; require(r.u64()==0 && r.u32()==2 && r.u32()==1 && r.u32()==2); r.end();
            require(store.channels()[0].id=="a" && store.channels()[1].name=="Temperature" && store.channels()[1].unit=="C" && store.sample_rate()==25);
            call(s.value,tcp::Data,samples(0,{12.5f,40}),true,true);
            call(s.value,tcp::Data,samples(1,{25,41,50,42}));
            auto points=store.query(0,0,10,100); require(points.size()==3 && points[0].value==12.5 && points[2].value==50 && std::abs(points[2].t-0.08)<1e-9);
            auto frozen=store.snapshot(0,10);
            call(s.value,tcp::Data,samples(3,{std::numeric_limits<float>::quiet_NaN(),1}),false); require(store.range().frame_count==3);
            tcp::Writer ch; ch.u32(1); call(s.value,tcp::ClearChannel,ch);
            require(store.query(0,0,10,100).empty() && store.query(1,0,10,100).size()==3);
            require(store.channel_first_frame(0)==3 && frozen.range().frame_count==3);
            call(s.value,tcp::Data,samples(3,{75,43})); require(store.query(0,0,10,100).size()==1);
            tcp::Writer bad; bad.u32(99); call(s.value,tcp::ClearChannel,bad,false);
            call(s.value,tcp::ClearSource,{}); require(store.query(0,0,10,100).empty() && store.query(1,0,10,100).empty());
            call(s.value,tcp::Data,samples(4,{88,44})); require(store.query(0,0,10,100).size()==1);
            std::this_thread::sleep_for(std::chrono::milliseconds(5200)); require(receiver.connected);
        }
        {
            tcp::Socket s; connect_client(s);
            call(s.value,tcp::Hello,hello());
            call(s.value,tcp::Register,schema(false,50),false); require(store.sample_rate()==25);
            auto ack=call(s.value,tcp::Register,schema()); tcp::Reader r{ack.payload}; require(r.u64()==5);
            call(s.value,tcp::Data,samples(4,{1,2}),false);
            call(s.value,tcp::Data,samples(5,{99,45})); require(receiver.frames==6);
            // Oversized frame must close without allocating or touching history.
            auto wire=tcp::encode({tcp::Data,id++,{}}); tcp::put32(wire.data()+16,tcp::max_payload+1);
            require(tcp::transfer(s.value,wire.data(),wire.size(),true,stop));
        }
        {
            tcp::Socket s; connect_client(s); call(s.value,tcp::Hello,hello());
            const auto start=std::chrono::steady_clock::now(); receiver.stop();
            require(std::chrono::steady_clock::now()-start<std::chrono::seconds(1));
        }
        std::cout<<"PASS protocol v2 registration/ACK, metadata/rate, fragmented frames, single/batch, clear scopes, idle/reconnect, schema/sequence/NaN/length rejection, bounded stop\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n'; return 1;}
}
