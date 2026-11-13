#pragma once
#include "data_core.hpp"

namespace wave {
std::vector<ChannelInfo> simulation_channels();

struct SourceStats {
    std::uint64_t produced_frames{};
    std::uint64_t skipped_frames{};
    double elapsed_seconds{};
};

// A replacement for future UART/RTT/network producers, with no UI dependency.
// Produces all channels at the store's sample rate, paced in 5 ms batches.
class SimulationSource {
public:
    explicit SimulationSource(DataStore& store);
    ~SimulationSource();
    SimulationSource(const SimulationSource&) = delete;
    SimulationSource& operator=(const SimulationSource&) = delete;
    void start();
    void stop();
    bool running() const;
    SourceStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wave
