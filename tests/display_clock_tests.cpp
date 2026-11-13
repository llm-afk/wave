#include "display_clock.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
bool near(double a, double b) { return std::abs(a-b) <= 1e-9 * std::max({1.0, std::abs(a), std::abs(b)}); }

int main() {
    try {
        {
            wave::DisplayClock clock;
            wave::DataRange range{false,0,0,0,0,1};
            double previous=0; int between_packets=0;
            for(int tick=0;tick<600;++tick) {
                const double now=tick*0.01;
                const bool arrived=tick%4==0;
                if(arrived) {range.end+=0.04; range.last_frame+=1920; range.frame_count+=1920;}
                const auto position=clock.update(range,48000,now);
                require(position<=range.end && position>=previous, "Display clock must not reverse or extrapolate");
                if(tick>50) {
                    require(position-previous<=0.011001, "Batched arrival must not cause a 40 ms display jump");
                    if(!arrived && position>previous) ++between_packets;
                    require(range.end-position<0.3, "Display latency must remain bounded");
                }
                previous=position;
            }
            require(clock.buffered() && between_packets>350, "Clock must advance smoothly between 25 Hz packets");
            for(int tick=600;tick<650;++tick) previous=clock.update(range,48000,tick*0.01);
            require(near(previous,range.end), "Underrun must stop at the last real sample");
            const auto stopped=clock.update(range,48000,6.5);
            require(near(stopped,previous), "No data must not invent future time");
            clock.update({},48000,6.51);
            require(!clock.buffered(), "Clear must reset display buffering");
            range={false,100,100.1,4800000,4804800,4801};
            require(near(clock.update(range,48000,6.52),100.1), "New segment must not retain the old playhead");
            clock.reset();
            range={false,0,0,0,0,1};
            for(int tick=0;tick<100;++tick) {
                if(tick<=12 && tick%4==0) range.end+=0.04;
                clock.update(range,48000,tick*0.01);
            }
            require(near(clock.position(),range.end), "A short stream must drain even during initial prebuffer");
            clock.reset();
            range={false,0,0,0,0,1}; previous=0;
            for(int tick=0;tick<1000;++tick) {
                // Alternating 30/50 ms arrivals, 40 ms of samples per packet.
                if(tick%8==0 || tick%8==3) range.end+=0.04;
                range.begin=std::max(0.0,range.end-2.0);
                const auto position=clock.update(range,48000,tick*0.01);
                require(position>=previous && position<=range.end, "Jitter and ring rollover must preserve time bounds");
                if(tick>80) require(position-previous<=0.011001, "Arrival jitter must not cause display jumps");
                previous=position;
            }
            range.begin=range.end-0.001;
            require(near(clock.update(range,48000,10),range.end), "Tiny history must safely fall back to received data");
            std::cout << "PASS adaptive display clock: 25 Hz batches, jitter, rollover, stream end and reset\n";
        }
        return 0;
    } catch(const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}
