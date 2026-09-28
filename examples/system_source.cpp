#include "tcp_source.hpp"
#include <windows.h>
#include <iostream>

std::uint64_t ticks(FILETIME t) { return (std::uint64_t(t.dwHighDateTime)<<32)|t.dwLowDateTime; }
int main(int argc, char** argv) {
    try {
        const unsigned batch=argc>1?unsigned(std::stoul(argv[1])):1;
        const unsigned limit=argc>2?unsigned(std::stoul(argv[2])):0;
        if(batch<1 || batch>10) throw std::runtime_error("Batch size must be 1..10");
        wave::tcp::Winsock winsock;
        std::atomic<bool> stop{false};
        unsigned sent=0;
        bool waiting_announced=false;
        std::cout << "Real CPU / memory usage -> 127.0.0.1:18765, 5 Hz, batch=" << batch << std::endl;
        while(!limit || sent<limit) {
            wave::tcp::Socket client{socket(AF_INET,SOCK_STREAM,IPPROTO_TCP)};
            sockaddr_in address{}; address.sin_family=AF_INET; address.sin_port=htons(wave::tcp::configured_port()); address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
            if(connect(client.value,reinterpret_cast<sockaddr*>(&address),sizeof(address))==SOCKET_ERROR) {
                if(limit) throw std::runtime_error("Wave receiver unavailable");
                if(!waiting_announced) { std::cout << "Waiting for Wave. Start Wave separately; this source will reconnect automatically." << std::endl; waiting_announced=true; }
                Sleep(1000); continue;
            }
            std::uint32_t request_id=1;
            std::uint64_t base=0;
            try {
                wave::tcp::Writer hello; hello.text("windows-system"); hello.text("Windows system monitor");
                wave::tcp::request(client.value,wave::tcp::Hello,request_id++,hello,stop);
                wave::tcp::Writer metadata; metadata.u32(2);
                for(const auto& channel: std::vector<wave::ChannelInfo>{{"CPU usage","%","cpu"},{"Memory usage","%","memory"}}) {
                    metadata.text(channel.id); metadata.text(channel.name); metadata.text(channel.unit);
                    metadata.u32(1); metadata.u32(1); metadata.f64(5);
                }
                const auto ack=wave::tcp::request(client.value,wave::tcp::Register,request_id++,metadata,stop);
                wave::tcp::Reader registered{ack.payload}; base=registered.u64();
            } catch(const std::exception& e) {
                std::cerr << "Registration: " << e.what() << std::endl;
                if(limit) throw;
                Sleep(1000); continue;
            }
            const auto send_batch=[&](std::uint64_t first,const std::vector<char>& values) {
                wave::tcp::Writer payload; payload.u64(first); payload.u32(unsigned(values.size()/8));
                payload.bytes.insert(payload.bytes.end(),values.begin(),values.end());
                try { wave::tcp::request(client.value,wave::tcp::Data,request_id++,payload,stop); return true; }
                catch(const std::exception& e) {std::cerr << e.what() << std::endl; return false;}
            };
            waiting_announced=false;
            std::cout << "Connected to Wave. Next sample index=" << base << std::endl;
            auto started=std::chrono::steady_clock::now();
            FILETIME idle{},kernel{},user{};
            if(!GetSystemTimes(&idle,&kernel,&user)) throw std::runtime_error("GetSystemTimes failed");
            auto previous_idle=ticks(idle), previous_total=ticks(kernel)+ticks(user);
            std::uint64_t next=0, first=0;
            std::vector<char> values;
            bool failed=false;
            while(!limit || sent<limit) {
                std::this_thread::sleep_until(started+std::chrono::milliseconds((next+1)*200));
                const auto slot=std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count()/200)-1;
                // Never invent catch-up samples after a scheduler delay. A new
                // segment is sent and the receiver records the skipped slots.
                if(slot!=next && !values.empty()) {
                    if(!send_batch(first,values)) { failed=true; break; }
                    values.clear();
                }
                if(values.empty()) first=base+slot;
                next=slot+1;
                if(!GetSystemTimes(&idle,&kernel,&user)) throw std::runtime_error("GetSystemTimes failed");
                const auto total=ticks(kernel)+ticks(user), idle_now=ticks(idle);
                const auto delta=total-previous_total;
                const float cpu=delta?float(100.0*(1.0-double(idle_now-previous_idle)/double(delta))):0;
                previous_idle=idle_now; previous_total=total;
                MEMORYSTATUSEX memory{}; memory.dwLength=sizeof(memory);
                if(!GlobalMemoryStatusEx(&memory)) throw std::runtime_error("GlobalMemoryStatusEx failed");
                const float used=float(100.0*(1.0-double(memory.ullAvailPhys)/double(memory.ullTotalPhys)));
                const auto offset=values.size(); values.resize(offset+8);
                wave::tcp::put32(values.data()+offset,std::bit_cast<std::uint32_t>(cpu));
                wave::tcp::put32(values.data()+offset+4,std::bit_cast<std::uint32_t>(used));
                ++sent;
                if(values.size()/8==batch || (limit && sent==limit)) {
                    if(!send_batch(first,values)) { failed=true; break; }
                    values.clear();
                    std::cout << "CPU=" << cpu << "% Memory=" << used << "% samples=" << sent << std::endl;
                }
            }
            if(failed && limit) throw std::runtime_error("Send failed");
            if(failed) { std::cout << "Wave disconnected. Retrying..." << std::endl; Sleep(1000); }
        }
        return 0;
    } catch(const std::exception& e) { std::cerr << e.what() << std::endl; return 1; }
}
