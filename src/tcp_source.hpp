#pragma once
#include "signal_hub.hpp"
#include "wave_protocol.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>
#include <unordered_map>
#include <cstdlib>
namespace wave::tcp {
// Optional process-local endpoint override for isolated integration tests.
inline unsigned short configured_port() {
    const char* value=std::getenv("WAVE_PORT"); if(!value || !*value) return port;
    const std::string text(value);
    if(text.find_first_not_of("0123456789")!=std::string::npos) throw std::runtime_error("Invalid WAVE_PORT");
    const auto number=std::stoul(text);
    if(!number || number>65535) throw std::runtime_error("Invalid WAVE_PORT");
    return static_cast<unsigned short>(number);
}
struct Winsock {Winsock(){WSADATA d{}; if(WSAStartup(MAKEWORD(2,2),&d)) throw std::runtime_error("WSAStartup failed");} ~Winsock(){WSACleanup();}};
struct Socket {SOCKET value{INVALID_SOCKET}; ~Socket(){if(value!=INVALID_SOCKET) closesocket(value);}};
inline bool transfer(SOCKET s,char* p,std::size_t size,bool sending,const std::atomic<bool>& stop,bool idle=false) {
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(size && !stop) {
        fd_set set; FD_ZERO(&set); FD_SET(s,&set); timeval t{0,100000};
        int ready=select(0,sending?nullptr:&set,sending?&set:nullptr,nullptr,&t);
        if(ready==SOCKET_ERROR || (!idle && std::chrono::steady_clock::now()>deadline)) return false;
        if(!ready) continue;
        int n=sending?send(s,p,int(size),0):recv(s,p,int(size),0); if(n<=0) return false;
        if(idle){idle=false; deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);}
        p+=n; size-=std::size_t(n);
    } return size==0;
}
inline bool send_message(SOCKET s,const Message& m,const std::atomic<bool>& stop) {auto b=encode(m); return transfer(s,b.data(),b.size(),true,stop);}
inline bool read_message(SOCKET s,Message& m,const std::atomic<bool>& stop,bool idle=true) {
    char h[20]; if(!transfer(s,h,20,false,stop,idle)) return false;
    if(get32(h)!=magic || get32(h+4)!=version || get32(h+16)>max_payload) throw std::runtime_error("Invalid frame header/version/length");
    m.type=get32(h+8); m.request=get32(h+12); m.payload.resize(get32(h+16));
    return transfer(s,m.payload.data(),m.payload.size(),false,stop);
}
// Each connection has one bounded in-flight request. UI polling services every
// connection once per frame; a slow or idle peer cannot block the others.
class Receiver {
    SignalHub owned_; SignalHub& hub_; Winsock winsock_; Socket listener_;
    std::atomic<bool> stop_{false}; std::thread acceptor_;
    struct Session {
        Socket socket; std::thread worker; std::mutex mutex; std::condition_variable cv;
        std::optional<Message> pending, result;
        std::atomic<bool> done{false}, registered{false};
        bool hello=false;
        std::string id,name;
        SignalHub::Source* source=nullptr;
        double arrival=0;
    };
    std::mutex sessions_mutex_;
    std::vector<std::unique_ptr<Session>> sessions_;
    std::unordered_map<std::string,Session*> owners_;
    std::optional<double> epoch_;
    double session_time(double arrival) {
        if(!epoch_) epoch_=arrival;
        return std::max(0.0,arrival-*epoch_);
    }
    Message dispatch(Session& session,const Message& m) {
        Writer reply,extra;
        try {
            if(!m.request) throw std::runtime_error("Request ID must be nonzero");
            Reader r{m.payload};
            if(m.type==Hello) {
                if(session.hello) throw std::runtime_error("HELLO already completed");
                auto id=r.text(), name=r.text(); r.end();
                if(id.empty() || name.empty()) throw std::runtime_error("Source ID/name required");
                if(owners_.contains(id)) throw std::runtime_error("Source ID is already connected");
                session.id=id; session.name=name; owners_[id]=&session; session.hello=true;
            } else if(m.type==Register) {
                if(!session.hello || session.registered) throw std::runtime_error("REGISTER requires HELLO; once per connection");
                auto count=r.u32(); if(!count || count>64) throw std::runtime_error("Channel count must be 1..64");
                std::vector<ChannelInfo> channels; std::unordered_set<std::string> ids;
                double rate=0; unsigned time_mode=0;
                for(unsigned i=0;i<count;++i) {
                    auto id=r.text(),name=r.text(),unit=r.text(); auto type=r.u32(),mode=r.u32(); auto hz=r.f64();
                    if(id.empty() || name.empty() || !ids.insert(id).second) throw std::runtime_error("Unique IDs and nonempty names required");
                    if(type!=1 || (mode!=1 && mode!=2) || (i && mode!=time_mode)) throw std::runtime_error("Channels must share uniform or receiver time mode");
                    if(!std::isfinite(hz) || (mode==1 ? hz<=0 || hz>1000000 : hz!=0) || (i && hz!=rate)) throw std::runtime_error("Invalid or mismatched sample rate");
                    rate=hz; time_mode=mode; channels.push_back({name,unit,id});
                } r.end();
                auto& source=hub_.register_source(session.id,session.name,rate,channels,time_mode==2);
                session.source=&source; source.online=true; session.registered=true;
                connected=true; ++connections;
                extra.u64(source.next); extra.u32(count); for(unsigned i=0;i<count;++i) extra.u32(i+1);
            } else {
                if(!session.registered) throw std::runtime_error("Register before data or clear commands");
                auto& source=*session.source; auto& store=*source.data;
                if(m.type==Data) {
                    if(store.reception_time()) throw std::runtime_error("Receiver time requires DATA_NOW");
                    const auto first=r.u64(); const auto count=r.u32();
                    if(!count || count>4096 || first<source.next || first>std::numeric_limits<std::uint64_t>::max()-count) throw std::runtime_error("Invalid count or sequence");
                    const auto n=std::size_t(count)*store.channels().size(); r.need(n*4);
                    std::vector<float> values; values.reserve(n);
                    for(std::size_t i=0;i<n;++i) values.push_back(r.f32()); r.end();
                    // Validate and commit before changing the mapping or sequence.
                    store.append(first,values);
                    if(!source.anchored) {
                        source.offset=session_time(session.arrival)-double(first+count-1)/store.sample_rate();
                        source.anchored=true;
                    }
                    gaps+=first-source.next; source.next=first+count; frames+=count; ++batches; extra.u64(source.next);
                } else if(m.type==DataNow) {
                    if(!store.reception_time()) throw std::runtime_error("DATA_NOW requires receiver time registration");
                    std::vector<float> values;
                    for(std::size_t i=0;i<store.channels().size();++i) values.push_back(r.f32()); r.end();
                    for(float v:values) if(!std::isfinite(v)) throw std::runtime_error("Samples must be finite");
                    const double timestamp=std::max(session_time(session.arrival),std::nextafter(source.last_time,std::numeric_limits<double>::infinity()));
                    store.append_received(source.next,values,timestamp); source.last_time=timestamp; source.anchored=true;
                    ++source.next; ++frames; ++batches; extra.u64(source.next); extra.f64(timestamp);
                } else if(m.type==ClearChannel) {
                    const auto handle=r.u32(); r.end();
                    if(!handle || handle>store.channels().size()) throw std::runtime_error("Unknown channel handle");
                    store.clear_channel(handle-1);
                } else if(m.type==ClearSource) {
                    r.end(); for(std::size_t i=0;i<store.channels().size();++i) store.clear_channel(i);
                } else throw std::runtime_error("Unknown message type");
            }
            reply.u32(0); reply.text(""); reply.bytes.insert(reply.bytes.end(),extra.bytes.begin(),extra.bytes.end());
        } catch(const std::exception& e) {++rejected; reply.u32(1); reply.text(e.what());}
        return {Reply,m.request,std::move(reply.bytes)};
    }
    void serve(Session& session) {
        try {
            while(!stop_) {
                Message request;
                if(!read_message(session.socket.value,request,stop_,session.registered.load())) break;
                const double arrival=std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
                std::unique_lock lock(session.mutex);
                session.arrival=arrival; session.pending=std::move(request); session.result.reset();
                session.cv.wait(lock,[&]{return stop_ || session.result.has_value();});
                if(stop_) break;
                auto reply=std::move(*session.result); session.result.reset(); lock.unlock();
                if(!send_message(session.socket.value,reply,stop_)) break;
            }
        } catch(...) {++rejected;}
        session.done=true;
    }
    void accept_loop() {
        while(!stop_) {
            fd_set set; FD_ZERO(&set); FD_SET(listener_.value,&set); timeval t{0,100000};
            if(select(0,&set,nullptr,nullptr,&t)<=0) continue;
            auto session=std::make_unique<Session>(); session->socket.value=accept(listener_.value,nullptr,nullptr);
            if(session->socket.value==INVALID_SOCKET) continue;
            std::lock_guard lock(sessions_mutex_);
            if(sessions_.size()>=32) continue;
            auto* ptr=session.get();
            sessions_.push_back(std::move(session));
            ptr->worker=std::thread([this,ptr]{serve(*ptr);});
        }
    }
    void start(unsigned short listen_port) {
        listener_.value=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP); BOOL exclusive=TRUE;
        setsockopt(listener_.value,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&exclusive),sizeof(exclusive));
        sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons(listen_port); a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        if(listener_.value==INVALID_SOCKET || bind(listener_.value,reinterpret_cast<sockaddr*>(&a),sizeof(a))==SOCKET_ERROR || listen(listener_.value,16)==SOCKET_ERROR) throw std::runtime_error("Wave receive port unavailable");
        acceptor_=std::thread([this]{accept_loop();});
    }
public:
    std::atomic<bool> connected{false};
    std::atomic<std::uint64_t> frames{0},batches{0},rejected{0},gaps{0},connections{0};
    explicit Receiver(SignalHub& hub,unsigned short listen_port=configured_port()):hub_(hub){start(listen_port);}
    explicit Receiver(DataStore& data,unsigned short listen_port=configured_port()):owned_(data),hub_(owned_){start(listen_port);}
    unsigned short listen_port() const {
        sockaddr_in address{}; int size=sizeof(address);
        if(getsockname(listener_.value,reinterpret_cast<sockaddr*>(&address),&size)!=0) throw std::runtime_error("Cannot read listen port");
        return ntohs(address.sin_port);
    }
    void poll() {
        std::lock_guard lock(sessions_mutex_);
        // Release all departed owners before processing new HELLO messages.
        std::erase_if(sessions_,[&](auto& session) {
            if(!session->done) return false;
            if(session->worker.joinable()) session->worker.join();
            if(session->hello) owners_.erase(session->id);
            if(session->source) session->source->online=false;
            return true;
        });
        bool any=false;
        for(auto& session:sessions_) {
            std::lock_guard request_lock(session->mutex);
            if(session->pending) {
                session->result=dispatch(*session,*session->pending); session->pending.reset(); session->cv.notify_all();
            }
            any=any || session->registered.load();
        }
        connected=any;
    }
    void stop() {
        stop_=true;
        if(acceptor_.joinable()) acceptor_.join();
        for(auto& session:sessions_) session->cv.notify_all();
        for(auto& session:sessions_) {
            if(session->worker.joinable()) session->worker.join();
            if(session->source) session->source->online=false;
        }
        connected=false;
    }
    ~Receiver(){stop();}
};
inline Message request(SOCKET s,std::uint32_t type,std::uint32_t id,const Writer& payload,const std::atomic<bool>& stop) {
    if(!send_message(s,{type,id,payload.bytes},stop)) throw std::runtime_error("Send failed");
    Message reply; if(!read_message(s,reply,stop,false) || reply.type!=Reply || reply.request!=id) throw std::runtime_error("Invalid/missing reply");
    Reader r{reply.payload}; auto code=r.u32(); auto error=r.text(); if(code) throw std::runtime_error(error);
    reply.payload.erase(reply.payload.begin(),reply.payload.begin()+r.pos); return reply;
}
}
