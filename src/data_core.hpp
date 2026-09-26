#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace wave {

struct ChannelInfo {
    std::string name;
    std::string unit;
};

struct PlotPoint {
    double t{};
    double value{};
};

struct DataRange {
    bool empty{true};
    double begin{};
    double end{};
    std::uint64_t first_frame{};
    std::uint64_t last_frame{};
    std::size_t frame_count{};
};

std::vector<ChannelInfo> simulation_channels();

namespace detail { struct SampleStorage; }

// Immutable, independently owned data. Copying this handle is inexpensive.
class FrozenData {
public:
    FrozenData() = default;
    const std::vector<ChannelInfo>& channels() const;
    double sample_rate() const;
    DataRange range() const;
    std::vector<PlotPoint> query(std::size_t channel, double tmin, double tmax,
                                 std::size_t pixel_width) const;

private:
    friend class DataStore;
    std::shared_ptr<const detail::SampleStorage> storage_;
};

// The sole boundary between producers and the UI. Values are interleaved by
// frame: [ch0, ch1, ...], [ch0, ch1, ...]. Metadata never changes after creation.
class DataStore {
public:
    explicit DataStore(double sample_rate = 20'000.0,
                       double history_seconds = 30.0,
                       std::vector<ChannelInfo> channels = simulation_channels());
    ~DataStore();
    DataStore(const DataStore&) = delete;
    DataStore& operator=(const DataStore&) = delete;

    const std::vector<ChannelInfo>& channels() const;
    double sample_rate() const;
    std::size_t capacity_frames() const;
    DataRange range() const;

    // A discontinuity in first_frame starts a new contiguous segment. Producers
    // must supply finite values; invalid batches are rejected before mutation.
    void append(std::uint64_t first_frame, std::span<const float> interleaved);
    void clear();

    // Returns at most 4 * max(1, pixel_width) points, retaining the first, last,
    // minimum and maximum of each bin in original time order. Small views
    // return original samples. Out-of-history requests return only available
    // data (or empty); range() reports the actual retained interval.
    std::vector<PlotPoint> query(std::size_t channel, double tmin, double tmax,
                                 std::size_t pixel_width) const;
    FrozenData snapshot(double tmin, double tmax) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

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
