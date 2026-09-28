#pragma once
#include <bit>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
namespace wave::tcp {
constexpr unsigned short port=18765;
constexpr std::uint32_t magic=0x32564157, version=2, max_payload=1024*1024;
enum Type : std::uint32_t { Hello=1, Register=2, Data=3, ClearChannel=4, ClearSource=5, DataNow=6, Reply=128 };
inline bool valid_text(const std::string& text) {
    for(std::size_t i=0;i<text.size();) {
        auto c=static_cast<unsigned char>(text[i++]);
        if(c<32 || c==127) return false;
        if(c<128) continue;
        unsigned n=0, value=0, minimum=0;
        if(c>=0xc2 && c<=0xdf){n=1; value=c&31; minimum=0x80;}
        else if(c>=0xe0 && c<=0xef){n=2; value=c&15; minimum=0x800;}
        else if(c>=0xf0 && c<=0xf4){n=3; value=c&7; minimum=0x10000;}
        else return false;
        if(n>text.size()-i) return false;
        while(n--){auto b=static_cast<unsigned char>(text[i++]); if((b&0xc0)!=0x80) return false; value=(value<<6)|(b&63);}
        if(value<minimum || value>0x10ffff || (value>=0xd800 && value<=0xdfff)) return false;
    } return text.find("##")==std::string::npos;
}
inline void put32(char* p,std::uint32_t v) {for(int i=0;i<4;++i) p[i]=char(v>>(8*i));}
inline std::uint32_t get32(const char* p) {std::uint32_t v=0; for(int i=0;i<4;++i) v|=std::uint32_t(static_cast<unsigned char>(p[i]))<<(8*i); return v;}
inline void put64(char* p,std::uint64_t v) {put32(p,std::uint32_t(v)); put32(p+4,std::uint32_t(v>>32));}
inline std::uint64_t get64(const char* p) {return get32(p)|(std::uint64_t(get32(p+4))<<32);}
struct Writer {
    std::vector<char> bytes;
    void u32(std::uint32_t v) {auto n=bytes.size(); bytes.resize(n+4); put32(bytes.data()+n,v);}
    void u64(std::uint64_t v) {auto n=bytes.size(); bytes.resize(n+8); put64(bytes.data()+n,v);}
    void f64(double v) {u64(std::bit_cast<std::uint64_t>(v));}
    void f32(float v) {u32(std::bit_cast<std::uint32_t>(v));}
    void text(const std::string& s) {u32(std::uint32_t(s.size())); bytes.insert(bytes.end(),s.begin(),s.end());}
};
struct Reader {
    const std::vector<char>& bytes; std::size_t pos=0;
    void need(std::size_t n) {if(n>bytes.size()-pos) throw std::runtime_error("Truncated payload");}
    std::uint32_t u32() {need(4); auto v=get32(bytes.data()+pos); pos+=4; return v;}
    std::uint64_t u64() {need(8); auto v=get64(bytes.data()+pos); pos+=8; return v;}
    double f64() {return std::bit_cast<double>(u64());}
    float f32() {return std::bit_cast<float>(u32());}
    std::string text() {auto n=u32(); if(n>256) throw std::runtime_error("Text exceeds 256 bytes"); need(n); std::string s(bytes.data()+pos,n); pos+=n; if(!valid_text(s)) throw std::runtime_error("Invalid UTF-8 text"); return s;}
    void end() {if(pos!=bytes.size()) throw std::runtime_error("Unexpected trailing payload");}
};
struct Message {std::uint32_t type{},request{}; std::vector<char> payload;};
inline std::vector<char> encode(const Message& m) {
    if(m.payload.size()>max_payload) throw std::runtime_error("Payload too large");
    std::vector<char> out(20); put32(out.data(),magic); put32(out.data()+4,version); put32(out.data()+8,m.type); put32(out.data()+12,m.request); put32(out.data()+16,std::uint32_t(m.payload.size()));
    out.insert(out.end(),m.payload.begin(),m.payload.end()); return out;
}
}
