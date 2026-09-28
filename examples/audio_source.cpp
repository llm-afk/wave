#include "tcp_source.hpp"
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <ksmedia.h>
#include <algorithm>
#include <cstring>
#include <deque>
#include <iostream>
#include <sstream>

namespace {
std::atomic<bool> quitting{false};
BOOL WINAPI console_control(DWORD event) {
    if(event==CTRL_C_EVENT || event==CTRL_BREAK_EVENT || event==CTRL_CLOSE_EVENT) {quitting=true; return TRUE;}
    return FALSE;
}
void check(HRESULT hr,const char* action) {
    if(FAILED(hr)) {std::ostringstream out; out<<action<<" failed: 0x"<<std::hex<<unsigned(hr); throw std::runtime_error(out.str());}
}
template<class T> struct ComPtr {T* p{}; ~ComPtr(){if(p) p->Release();} T* operator->() const{return p;}};
struct ComScope {ComScope(){check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"COM");} ~ComScope(){CoUninitialize();}};
struct Format {WAVEFORMATEX* p{}; ~Format(){CoTaskMemFree(p);}};
struct Block {std::uint64_t first{}; std::vector<float> values;};
class Capture {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    Format format;
    bool floating=false;
    std::mutex mutex;
    std::deque<Block> queue;
    std::size_t queued=0;
    std::thread worker;
    std::atomic<bool> stopping{false};
    float sample(const BYTE* p) const {
        if(floating){float f; std::memcpy(&f,p,4); if(!std::isfinite(f)) throw std::runtime_error("Nonfinite audio sample"); return f;}
        if(format.p->wBitsPerSample==16){std::int16_t v; std::memcpy(&v,p,2); return v/32768.0f;}
        if(format.p->wBitsPerSample==24){int v=p[0]|(int(p[1])<<8)|(int(p[2])<<16); if(v&0x800000) v-=0x1000000; return v/8388608.0f;}
        std::int32_t v; std::memcpy(&v,p,4); return float(v/2147483648.0);
    }
    void run() {
        try {
            ComScope com;
            while(!stopping && !quitting) {
                UINT32 packet=0; check(capture->GetNextPacketSize(&packet),"GetNextPacketSize");
                if(!packet){Sleep(3); continue;}
                BYTE* bytes=nullptr; UINT32 frames=0; DWORD flags=0; UINT64 position=0;
                check(capture->GetBuffer(&bytes,&frames,&flags,&position,nullptr),"GetBuffer");
                struct Release {IAudioCaptureClient* p; UINT32 n; ~Release(){p->ReleaseBuffer(n);}} release{capture.p,frames};
                if(flags&AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR){discarded+=frames; continue;}
                if(flags&AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) ++discontinuities;
                Block block; block.first=position; block.values.resize(std::size_t(frames)*channels());
                float maximum=0;
                for(std::size_t i=0;i<block.values.size();++i) {
                    block.values[i]=(flags&AUDCLNT_BUFFERFLAGS_SILENT)?0.0f:sample(bytes+i*(format.p->wBitsPerSample/8));
                    maximum=std::max(maximum,std::abs(block.values[i]));
                }
                if(maximum>peak.load()) peak=maximum;
                acquired+=frames;
                std::lock_guard lock(mutex);
                queued+=frames; queue.push_back(std::move(block));
                while(queued>std::size_t(rate())*2 && !queue.empty()) {
                    auto n=queue.front().values.size()/channels(); queued-=n; discarded+=n; queue.pop_front();
                }
            }
        } catch(const std::exception& e){std::cerr<<e.what()<<std::endl; failed=true;}
    }
public:
    std::atomic<std::uint64_t> acquired{0},discarded{0},discontinuities{0};
    std::atomic<float> peak{0}; std::atomic<bool> failed{false};
    Capture() {
        check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(&enumerator.p)),"Audio enumeration");
        check(enumerator->GetDefaultAudioEndpoint(eRender,eConsole,&device.p),"Default playback device");
        check(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(&client.p)),"Audio activation");
        check(client->GetMixFormat(&format.p),"GetMixFormat");
        WORD tag=format.p->wFormatTag;
        if(tag==WAVE_FORMAT_EXTENSIBLE) {
            if(format.p->cbSize<22) throw std::runtime_error("Invalid extended audio format");
            const auto* ext=reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format.p);
            const GUID float_subtype={STATIC_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT};
            const GUID pcm_subtype={STATIC_KSDATAFORMAT_SUBTYPE_PCM};
            if(IsEqualGUID(ext->SubFormat,float_subtype)) tag=WAVE_FORMAT_IEEE_FLOAT;
            else if(IsEqualGUID(ext->SubFormat,pcm_subtype)) tag=WAVE_FORMAT_PCM;
        }
        floating=tag==WAVE_FORMAT_IEEE_FLOAT;
        if(!channels() || channels()>64 || !rate() || (floating?format.p->wBitsPerSample!=32:tag!=WAVE_FORMAT_PCM || (format.p->wBitsPerSample!=16 && format.p->wBitsPerSample!=24 && format.p->wBitsPerSample!=32)) || format.p->nBlockAlign!=channels()*(format.p->wBitsPerSample/8))
            throw std::runtime_error("Unsupported playback mix format");
        check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_LOOPBACK,1000000,0,format.p,nullptr),"Loopback initialize");
        check(client->GetService(__uuidof(IAudioCaptureClient),reinterpret_cast<void**>(&capture.p)),"Capture service");
    }
    unsigned rate() const {return format.p->nSamplesPerSec;}
    unsigned channels() const {return format.p->nChannels;}
    void start(){check(client->Start(),"Audio start"); worker=std::thread([this]{run();});}
    void stop(){stopping=true; if(worker.joinable()) worker.join(); client->Stop();}
    ~Capture(){stop();}
    void discard_pending(){std::lock_guard lock(mutex); queue.clear(); queued=0;}
    bool take(Block& result,std::size_t target,bool partial) {
        std::lock_guard lock(mutex);
        if(queue.empty() || (queued<target && !partial)) return false;
        result={}; result.first=queue.front().first;
        while(!queue.empty() && result.values.size()/channels()<target) {
            auto& front=queue.front();
            const auto used=result.values.size()/channels();
            if(front.first!=result.first+used) break;
            const auto count=std::min(target-used,front.values.size()/channels());
            result.values.insert(result.values.end(),front.values.begin(),front.values.begin()+count*channels());
            front.values.erase(front.values.begin(),front.values.begin()+count*channels());
            front.first+=count; queued-=count;
            if(front.values.empty()) queue.pop_front();
        }
        return !result.values.empty();
    }
};
}

int main(int argc,char** argv) {
    try {
        bool probe=false; double seconds=0;
        for(int i=1;i<argc;++i) {
            const std::string arg=argv[i];
            if(arg=="--probe") probe=true;
            else if(arg=="--seconds" && i+1<argc) {seconds=std::stod(argv[++i]); if(!std::isfinite(seconds)||seconds<=0) throw std::runtime_error("Invalid duration");}
            else throw std::runtime_error("Usage: AudioSource.exe [--probe] [--seconds N]");
        }
        SetConsoleCtrlHandler(console_control,TRUE);
        ComScope com; Capture audio;
        const auto target=std::min({std::size_t{4096},std::size_t((wave::tcp::max_payload-12)/(audio.channels()*4)),std::max(std::size_t{1},std::size_t(audio.rate()/25))});
        std::cout<<"Playback loopback: "<<audio.rate()<<" Hz, "<<audio.channels()<<" channels; batch="<<target<<" frames. No microphone capture or audio file recording."<<std::endl;
        if(probe) return 0;
        wave::tcp::Winsock winsock; audio.start();
        const auto started=std::chrono::steady_clock::now();
        const auto finished=[&]{return quitting.load() || audio.failed.load() || (seconds>0 && std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count()>=seconds);};
        std::uint64_t sent=0,packets=0; bool waiting=false;
        while(!finished()) {
            wave::tcp::Socket socket{::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP)};
            sockaddr_in address{}; address.sin_family=AF_INET; address.sin_port=htons(wave::tcp::configured_port()); address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
            if(connect(socket.value,reinterpret_cast<sockaddr*>(&address),sizeof(address))==SOCKET_ERROR) {
                if(!waiting){std::cout<<"Waiting for Wave (run.cmd). Close other sources first."<<std::endl; waiting=true;}
                Sleep(500); continue;
            }
            try {
                std::uint32_t id=1; wave::tcp::Writer hello; hello.text("wasapi-playback"); hello.text("Playback audio");
                wave::tcp::request(socket.value,wave::tcp::Hello,id++,hello,quitting);
                wave::tcp::Writer metadata; metadata.u32(audio.channels());
                for(unsigned ch=0;ch<audio.channels();++ch) {
                    metadata.text("audio."+std::to_string(ch+1));
                    metadata.text(audio.channels()==2?(ch?"Audio Right":"Audio Left"):"Audio "+std::to_string(ch+1));
                    metadata.text(""); metadata.u32(1); metadata.u32(1); metadata.f64(audio.rate());
                }
                const auto reply=wave::tcp::request(socket.value,wave::tcp::Register,id++,metadata,quitting);
                wave::tcp::Reader r{reply.payload}; const auto base=r.u64();
                audio.discard_pending(); std::optional<std::uint64_t> origin;
                std::uint64_t next=base;
                waiting=false; std::cout<<"Registered. Select audio channels in Wave, then play music/video. No playback may mean no samples."<<std::endl;
                auto last=std::chrono::steady_clock::now(), report=last;
                while(!finished()) {
                    const auto now=std::chrono::steady_clock::now(); Block block;
                    if(!audio.take(block,target,now-last>std::chrono::milliseconds(200))){Sleep(3); continue;}
                    if(!origin) origin=block.first;
                    if(block.first<*origin || base+block.first-*origin<next) throw std::runtime_error("Audio clock discontinuity; reconnecting");
                    const auto first=base+block.first-*origin;
                    wave::tcp::Writer payload; payload.u64(first); payload.u32(unsigned(block.values.size()/audio.channels()));
                    for(float value:block.values) payload.f32(value);
                    wave::tcp::request(socket.value,wave::tcp::Data,id++,payload,quitting);
                    sent+=block.values.size()/audio.channels(); ++packets; next=first+block.values.size()/audio.channels(); last=now;
                    if(now-report>std::chrono::seconds(1)){std::cout<<"sent="<<sent<<" packets="<<packets<<" dropped="<<audio.discarded<<" peak="<<audio.peak<<std::endl; report=now;}
                }
            } catch(const std::exception& e){if(!quitting) std::cerr<<e.what()<<" (restart Wave to switch from another source)"<<std::endl; Sleep(500);}
        }
        audio.stop();
        std::cout<<"SUMMARY rate="<<audio.rate()<<" channels="<<audio.channels()<<" captured="<<audio.acquired<<" sent="<<sent<<" packets="<<packets<<" dropped="<<audio.discarded<<" discontinuities="<<audio.discontinuities<<" peak="<<audio.peak<<std::endl;
        return audio.failed?1:0;
    } catch(const std::exception& e){std::cerr<<e.what()<<std::endl; return 1;}
}
