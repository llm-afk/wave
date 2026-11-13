#pragma once
#include "data_core.hpp"
#include "display_clock.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace wave {
inline DataRange merge_ranges(DataRange a, DataRange b) {
    if(a.empty) return b;
    if(b.empty) return a;
    a.begin=std::min(a.begin,b.begin); a.end=std::max(a.end,b.end);
    a.first_frame=std::min(a.first_frame,b.first_frame); a.last_frame=std::max(a.last_frame,b.last_frame);
    a.frame_count=std::max(a.frame_count,b.frame_count); return a;
}
struct SignalBinding {std::size_t source, channel;};
class SignalSnapshot {
    friend class SignalHub;
    std::vector<FrozenData> data_;
    std::vector<double> offsets_;
    std::vector<SignalBinding> bindings_;
public:
    double sample_rate(std::size_t channel) const {
        return data_.at(bindings_.at(channel).source).sample_rate();
    }
    DataRange channel_range(std::size_t channel, std::uint64_t cutoff=0) const {
        if(channel>=bindings_.size()) return {};
        const auto b=bindings_[channel]; const auto& data=data_[b.source]; auto r=data.range();
        if(r.empty || cutoff>r.last_frame) return {};
        r.first_frame=std::max(r.first_frame,cutoff); r.frame_count=r.last_frame-r.first_frame+1;
        r.begin=data.frame_time(r.first_frame)+offsets_[b.source]; r.end+=offsets_[b.source]; return r;
    }
    DataRange range() const {
        DataRange r; for(std::size_t i=0;i<bindings_.size();++i) r=merge_ranges(r,channel_range(i)); return r;
    }
    std::vector<PlotPoint> query(std::size_t channel,double begin,double end,std::size_t width,std::uint64_t cutoff=0,bool include_neighbors=false) const {
        if(channel>=bindings_.size()) return {};
        const auto b=bindings_[channel]; const double offset=offsets_[b.source];
        auto points=data_[b.source].query(b.channel,begin-offset,end-offset,width,cutoff,include_neighbors);
        for(auto& p:points) p.t+=offset;
        return points;
    }
};

// UI-thread-owned registry. Network workers only enqueue messages; all writes,
// metadata publication and snapshots are serialized by the UI's Receiver::poll.
class SignalHub {
public:
    struct Source {
        std::string id, name;
        std::unique_ptr<DataStore> owned;
        DataStore* data=nullptr;
        bool online=false, anchored=false;
        double offset=0, last_time=-1;
        std::uint64_t next=0;
        DisplayClock clock;
    };
private:
    std::vector<std::unique_ptr<Source>> sources_;
    std::vector<ChannelInfo> channels_;
    std::vector<SignalBinding> bindings_;
    std::size_t capacity_=default_capacity_frames;
    DataStore* legacy_=nullptr;
public:
    SignalHub()=default;
    explicit SignalHub(DataStore& data):capacity_(data.capacity_frames()),legacy_(&data) {
        auto s=std::make_unique<Source>(); s->data=&data; s->online=true; s->anchored=true;
        sources_.push_back(std::move(s)); sync();
    }
    bool legacy() const {return legacy_!=nullptr;}
    void sync() {
        if(legacy_) capacity_=legacy_->capacity_frames();
        if(legacy_ && sources_.size()==1) {
            const auto& metadata=legacy_->channels();
            if(metadata.size()==channels_.size() &&
               std::equal(metadata.begin(),metadata.end(),channels_.begin(),[](const auto& a,const auto& b) {
                   return a.name==b.name && a.unit==b.unit && a.id==b.id;
               })) return;
        }
        channels_.clear(); bindings_.clear();
        for(std::size_t i=0;i<sources_.size();++i) {
            const auto& c=sources_[i]->data->channels();
            for(std::size_t j=0;j<c.size();++j) {channels_.push_back(c[j]); bindings_.push_back({i,j});}
        }
    }
    Source* find(const std::string& id) {
        for(auto& s:sources_) if(s->id==id) return s.get(); return nullptr;
    }
    Source& register_source(const std::string& id,const std::string& name,double rate,
                            const std::vector<ChannelInfo>& metadata,bool received) {
        if(auto* s=find(id)) {
            const auto& old=s->data->channels();
            if(s->data->sample_rate()!=rate || s->data->reception_time()!=received || old.size()!=metadata.size())
                throw std::runtime_error("Reconnect schema mismatch");
            for(std::size_t i=0;i<old.size();++i)
                if(old[i].id!=metadata[i].id || old[i].name!=metadata[i].name || old[i].unit!=metadata[i].unit)
                    throw std::runtime_error("Reconnect metadata mismatch");
            return *s;
        }
        if(channels_.size()+metadata.size()>256 || sources_.size()>=32) throw std::runtime_error("Signal/source limit reached");
        if((channels_.size()+metadata.size())*double(capacity_)*12 > 512.0*1024*1024)
            throw std::runtime_error("Buffer memory budget exceeded");
        if(legacy_ && sources_[0]->id.empty()) {
            auto& s=*sources_[0]; s.data->configure(rate,metadata,received); s.id=id; s.name=name; sync(); return s;
        }
        auto s=std::make_unique<Source>(); s->id=id; s->name=name;
        s->owned=std::make_unique<DataStore>(1,double(capacity_),std::vector<ChannelInfo>{}); s->data=s->owned.get();
        s->data->configure(rate,metadata,received); sources_.push_back(std::move(s)); sync(); return *sources_.back();
    }
    const std::vector<ChannelInfo>& channels() const {return channels_;}
    std::size_t source_count() const {return sources_.size();}
    std::size_t capacity_frames() const {return capacity_;}
    const Source& source(std::size_t channel) const {return *sources_.at(bindings_.at(channel).source);}
    double sample_rate() const {
        double rate=0; for(const auto& s:sources_) rate=std::max(rate,s->data->sample_rate()); return rate;
    }
    bool reception_time(std::size_t channel) const {return source(channel).data->reception_time();}
    bool reception_time() const {return sample_rate()==0;}
    bool online(std::size_t channel) const {return source(channel).online;}
    std::string label(std::size_t channel) const {
        const auto& name=channels_[channel].name;
        std::size_t count=0,ordinal=0;
        for(std::size_t i=0;i<channels_.size();++i) if(channels_[i].name==name) {++count; if(i<=channel) ++ordinal;}
        return count>1 ? name+" ("+std::to_string(ordinal)+")" : name;
    }
    std::uint64_t channel_first_frame(std::size_t channel) const {
        const auto b=bindings_.at(channel); return sources_[b.source]->data->channel_first_frame(b.channel);
    }
    DataRange channel_range(std::size_t channel,std::uint64_t cutoff=0) const {
        if(channel>=bindings_.size()) return {};
        const auto& s=source(channel); auto r=s.data->range();
        if(r.empty || cutoff>r.last_frame) return {};
        r.first_frame=std::max(r.first_frame,cutoff); r.frame_count=r.last_frame-r.first_frame+1;
        r.begin=s.data->frame_time(r.first_frame)+s.offset; r.end+=s.offset; return r;
    }
    DataRange range() const {
        DataRange r; for(std::size_t i=0;i<channels_.size();++i) r=merge_ranges(r,channel_range(i)); return r;
    }
    std::vector<PlotPoint> query(std::size_t channel,double begin,double end,std::size_t width,std::uint64_t cutoff=0,bool include_neighbors=false) const {
        if(channel>=bindings_.size()) return {};
        const auto b=bindings_[channel]; const auto& s=*sources_[b.source];
        auto points=s.data->query(b.channel,begin-s.offset,end-s.offset,width,cutoff,include_neighbors);
        for(auto& p:points) p.t+=s.offset; return points;
    }
    SignalSnapshot snapshot(double begin,double end) const {
        SignalSnapshot result; result.bindings_=bindings_;
        for(const auto& s:sources_) {
            result.data_.push_back(s->data->snapshot(begin-s->offset,end-s->offset)); result.offsets_.push_back(s->offset);
        } return result;
    }
    void clear() {for(auto& s:sources_) {s->data->clear(); s->clock.reset();}}
    void set_capacity_frames(std::size_t frames) {
        if(frames<8 || frames>2000000 || channels_.size()*double(frames)*12 > 512.0*1024*1024)
            throw std::runtime_error("Buffer capacity exceeds limit");
        // Prepare every replacement before committing, so allocation failure
        // cannot leave the sources with different capacities.
        std::vector<std::unique_ptr<DataStore>> replacements;
        if(legacy_) {legacy_->set_capacity_frames(frames); capacity_=frames; return;}
        for(const auto& s:sources_) {
            auto copy=s->data->resized_copy(frames); replacements.push_back(std::move(copy));
        }
        for(std::size_t i=0;i<sources_.size();++i) {sources_[i]->owned=std::move(replacements[i]); sources_[i]->data=sources_[i]->owned.get();}
        capacity_=frames;
    }
    void update_clocks(double now) {
        for(auto& s:sources_) {
            if(s->data->reception_time()) s->clock.reset();
            s->clock.update(s->data->range(),s->data->sample_rate(),now);
        }
    }
    double position(const std::vector<bool>& selected) const {
        double result=-std::numeric_limits<double>::infinity();
        bool live=false;
        for(std::size_t i=0;i<channels_.size();++i) if(i<selected.size() && selected[i]) {
            const auto& s=source(i); live |= s.online && !s.data->range().empty;
        }
        for(std::size_t i=0;i<channels_.size();++i) if(i<selected.size() && selected[i]) {
            const auto& s=source(i);
            if(live && !s.online) continue;
            if(!s.data->range().empty) result=std::max(result,s.clock.position()+s.offset);
        }
        return std::isfinite(result)?result:0;
    }
};
}
