#pragma once
#include "data_core.hpp"
#include <algorithm>
#include <cmath>

namespace wave {
// A display-only playhead. Sampling values and source indices never change.
// Learn short, regularly arriving batches; retain a small adaptive look-behind.
class DisplayClock {
    bool initialized_=false, active_=false, playing_=false;
    unsigned observations_=0;
    double last_tick_=0, last_arrival_=0, latest_=0, position_=0;
    double cadence_=0.04, jitter_=0, sample_rate_=0;
public:
    double position() const {return position_;}
    bool buffered() const {return active_;}
    void reset(){*this=DisplayClock{};}
    double update(DataRange range,double sample_rate,double now) {
        if(range.empty){reset(); return 0;}
        if(!initialized_ || sample_rate!=sample_rate_ || range.end<latest_ ||
           range.begin>position_ || now<last_tick_ || now-last_tick_>0.5) {
            reset(); initialized_=true; sample_rate_=sample_rate;
            position_=latest_=range.end; last_arrival_=last_tick_=now; return position_;
        }
        const double dt=std::clamp(now-last_tick_,0.0,0.1); last_tick_=now;
        if(range.end>latest_) {
            const double source_step=range.end-latest_, arrival_step=now-last_arrival_;
            // Offline loads/large discontinuities should not slowly replay.
            if(source_step>0.5 || arrival_step>0.75) {
                active_=playing_=false; observations_=0;
            } else if(source_step>=0.008 && source_step<=0.25 && arrival_step>0) {
                const double interval=std::max(source_step,arrival_step);
                if(observations_==0) cadence_=interval;
                jitter_+=0.1*(std::abs(arrival_step-cadence_)-jitter_);
                cadence_+=0.1*(interval-cadence_);
                ++observations_;
                if(observations_>=3) active_=true;
            }
            latest_=range.end; last_arrival_=now;
        }
        if(!active_){position_=range.end; return position_;}
        const double retained=range.end-range.begin;
        // A tiny user-selected history cannot support a delayed playhead.
        if(retained<cadence_*1.25){active_=playing_=false; observations_=0; position_=range.end; return position_;}
        const double reserve=std::min(std::clamp(2*cadence_+2*jitter_,0.04,0.25),retained*0.65);
        if(!playing_) {
            if(range.end-position_>=reserve || now-last_arrival_>=reserve) playing_=true;
            else return position_;
        }
        const double ahead=range.end-position_;
        // Slow rate correction avoids a jump whenever a packet changes the
        // measured reserve. Never extrapolate beyond actually received data.
        const double speed=std::clamp(1.0+(ahead-reserve)*0.5,0.9,1.1);
        position_=std::min(range.end,position_+dt*speed);
        if(position_>=range.end) playing_=false;
        return position_;
    }
};
}
