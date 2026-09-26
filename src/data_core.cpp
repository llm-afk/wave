#include "data_core.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <numbers>
#include <stdexcept>
#include <thread>
#include <utility>

namespace wave {
namespace detail {

struct Extremes {
    float min_value{};
    float max_value{};
    std::uint64_t min_frame{};
    std::uint64_t max_frame{};
};

struct SummaryLevel {
    std::size_t block_size{};
    std::size_t block_count{};
    std::vector<Extremes> values;
};

struct SampleStorage {
    double sample_rate;
    std::size_t capacity;
    std::vector<ChannelInfo> channels;
    std::vector<float> values;
    std::vector<SummaryLevel> summaries;
    std::uint64_t first_frame{};
    std::uint64_t next_frame{};
    std::size_t count{};

    SampleStorage(double rate, std::size_t frame_capacity,
                  std::vector<ChannelInfo> metadata)
        : sample_rate(rate), capacity(frame_capacity), channels(std::move(metadata)),
          values(capacity * channels.size()) {
        // Fixed absolute-frame blocks form a small extrema pyramid. Queries
        // combine summaries, never scan/copy the entire retained history.
        for (std::size_t block = 16; block <= capacity;) {
            const auto slots = capacity / block + 2;
            summaries.push_back({block, slots,
                                 std::vector<Extremes>(slots * channels.size())});
            if (block > capacity / 16) break;
            block *= 16;
        }
    }

    float value(std::uint64_t frame, std::size_t channel) const {
        return values[(frame % capacity) * channels.size() + channel];
    }

    DataRange range() const {
        if (!count) return {};
        return {false, static_cast<double>(first_frame) / sample_rate,
                static_cast<double>(next_frame - 1) / sample_rate,
                first_frame, next_frame - 1, count};
    }

    void append(std::uint64_t start, std::span<const float> input) {
        const auto channel_count = channels.size();
        const auto frame_count = input.size() / channel_count;
        if (!frame_count) return;
        // A gap is visible as a new range rather than a fabricated connected
        // line across missing samples. Partial summary blocks are reinitialized.
        const bool discontinuity = !count || start != next_frame;
        if (discontinuity) {
            count = 0;
            first_frame = start;
            next_frame = start;
        }
        for (std::size_t offset = 0; offset < frame_count; ++offset) {
            const auto frame = start + offset;
            const auto slot = (frame % capacity) * channel_count;
            std::copy_n(input.begin() + offset * channel_count, channel_count, values.begin() + slot);
            for (auto& level : summaries) {
                const auto summary_slot = ((frame / level.block_size) % level.block_count) * channel_count;
                const bool reset = frame % level.block_size == 0 || (discontinuity && offset == 0);
                for (std::size_t ch = 0; ch < channel_count; ++ch) {
                    const float sample = input[offset * channel_count + ch];
                    auto& summary = level.values[summary_slot + ch];
                    if (reset) {
                        summary = {sample, sample, frame, frame};
                    } else {
                        if (sample < summary.min_value) {
                            summary.min_value = sample;
                            summary.min_frame = frame;
                        }
                        if (sample > summary.max_value) {
                            summary.max_value = sample;
                            summary.max_frame = frame;
                        }
                    }
                }
            }
        }
        next_frame = start + frame_count;
        count = static_cast<std::size_t>(std::min<std::uint64_t>(
            capacity, next_frame - first_frame));
        first_frame = next_frame - count;
    }

    std::pair<std::uint64_t, std::uint64_t> interval(double tmin, double tmax) const {
        if (!count || std::isnan(tmin) || std::isnan(tmax) || tmin > tmax)
            return {0, 0};
        const auto available = range();
        if (tmax < available.begin || tmin > available.end) return {0, 0};
        std::uint64_t first = first_frame;
        std::uint64_t end = next_frame;
        // Accommodate round-off when a timestamp returned by range/query is
        // passed back into this API. This tolerance is far below one sample.
        const auto tolerance = [](long double x) {
            return 8 * std::numeric_limits<double>::epsilon() *
                   std::max(1.0L, std::abs(x));
        };
        if (tmin > available.begin) {
            const long double scaled = static_cast<long double>(tmin) * sample_rate;
            first = static_cast<std::uint64_t>(std::ceil(scaled - tolerance(scaled)));
        }
        if (tmax < available.end) {
            const long double scaled = static_cast<long double>(tmax) * sample_rate;
            end = static_cast<std::uint64_t>(std::floor(scaled + tolerance(scaled))) + 1;
        }
        first = std::clamp(first, first_frame, next_frame);
        end = std::clamp(end, first_frame, next_frame);
        return first < end ? std::pair{first, end} : std::pair<std::uint64_t, std::uint64_t>{0, 0};
    }

    Extremes extremes(std::size_t channel, std::uint64_t first, std::uint64_t end) const {
        const auto initial = value(first, channel);
        Extremes result{initial, initial, first, first};
        const auto include = [&result](const Extremes& item) {
            if (item.min_value < result.min_value) {
                result.min_value = item.min_value;
                result.min_frame = item.min_frame;
            }
            if (item.max_value > result.max_value) {
                result.max_value = item.max_value;
                result.max_frame = item.max_frame;
            }
        };
        for (auto frame = first; frame < end;) {
            bool summarized = false;
            for (auto it = summaries.rbegin(); it != summaries.rend(); ++it) {
                if (end - frame >= it->block_size && frame % it->block_size == 0) {
                    include(it->values[((frame / it->block_size) % it->block_count) *
                                       channels.size() + channel]);
                    frame += it->block_size;
                    summarized = true;
                    break;
                }
            }
            if (!summarized) {
                const auto sample = value(frame, channel);
                include({sample, sample, frame, frame});
                ++frame;
            }
        }
        return result;
    }

    std::vector<PlotPoint> query(std::size_t channel, double tmin, double tmax,
                                  std::size_t pixel_width) const {
        if (channel >= channels.size()) throw std::out_of_range("Invalid channel index");
        const auto [first, end] = interval(tmin, tmax);
        if (first == end) return {};
        const auto n = static_cast<std::size_t>(end - first);
        const auto pixels = std::clamp(pixel_width, std::size_t{1}, n);
        std::vector<PlotPoint> result;
        const auto push = [&](std::uint64_t frame) {
            result.push_back({static_cast<double>(frame) / sample_rate, value(frame, channel)});
        };
        if ((n + 3) / 4 <= pixels) {
            result.reserve(n);
            for (auto frame = first; frame < end; ++frame) push(frame);
            return result;
        }
        result.reserve(pixels * 4);
        // Integer bin arithmetic avoids timestamp drift and duplicate bins.
        for (std::size_t bin = 0; bin < pixels; ++bin) {
            const auto lo = first + (n / pixels) * bin + (n % pixels) * bin / pixels;
            const auto hi = first + (n / pixels) * (bin + 1) +
                            (n % pixels) * (bin + 1) / pixels;
            const auto summary = extremes(channel, lo, hi);
            std::array<std::uint64_t, 4> indices{lo, summary.min_frame, summary.max_frame, hi - 1};
            std::sort(indices.begin(), indices.end());
            const auto unique_end = std::unique(indices.begin(), indices.end());
            for (auto it = indices.begin(); it != unique_end; ++it) push(*it);
        }
        return result;
    }
};

} // namespace detail

namespace {
std::size_t checked_capacity(double rate, double seconds, std::size_t channels) {
    if (!std::isfinite(rate) || rate <= 0 || !std::isfinite(seconds) || seconds <= 0 || !channels)
        throw std::invalid_argument("Sample rate, history duration and channel count must be positive");
    const long double exact_frames = static_cast<long double>(rate) * seconds;
    const long double rounding_slack = 8 * std::numeric_limits<double>::epsilon() *
                                       std::max(1.0L, std::abs(exact_frames));
    const long double frames = std::max(1.0L, std::ceil(exact_frames - rounding_slack));
    const auto limit = std::numeric_limits<std::size_t>::max() / channels / sizeof(float);
    if (frames > static_cast<long double>(limit))
        throw std::length_error("History buffer is too large");
    return static_cast<std::size_t>(frames);
}
} // namespace

struct DataStore::Impl {
    mutable std::mutex mutex;
    detail::SampleStorage storage;
    Impl(double rate, double seconds, std::vector<ChannelInfo> metadata)
        : storage(rate, checked_capacity(rate, seconds, metadata.size()), metadata) {}
};

std::vector<ChannelInfo> simulation_channels() {
    return {{"Phase A", "A"}, {"Phase B", "A"}, {"Phase C", "A"},
            {"PWM monitor", "V"}, {"Triangle", "V"}, {"Modulated", "A"},
            {"Single-sample spikes", "A"}, {"Noise + ripple", "V"}};
}

DataStore::DataStore(double rate, double seconds, std::vector<ChannelInfo> channels)
    : impl_(std::make_unique<Impl>(rate, seconds, std::move(channels))) {}
DataStore::~DataStore() = default;
const std::vector<ChannelInfo>& DataStore::channels() const { return impl_->storage.channels; }
double DataStore::sample_rate() const { return impl_->storage.sample_rate; }
std::size_t DataStore::capacity_frames() const { return impl_->storage.capacity; }

DataRange DataStore::range() const {
    const std::lock_guard lock(impl_->mutex);
    return impl_->storage.range();
}

void DataStore::append(std::uint64_t first_frame, std::span<const float> values) {
    const auto channel_count = channels().size();
    if (values.size() % channel_count != 0)
        throw std::invalid_argument("Batch does not contain whole interleaved frames");
    const auto frames = values.size() / channel_count;
    if (frames > std::numeric_limits<std::uint64_t>::max() - first_frame)
        throw std::overflow_error("Sample index overflow");
    if (std::any_of(values.begin(), values.end(), [](float value) { return !std::isfinite(value); }))
        throw std::invalid_argument("Samples must be finite");
    const std::lock_guard lock(impl_->mutex);
    impl_->storage.append(first_frame, values);
}

void DataStore::clear() {
    const std::lock_guard lock(impl_->mutex);
    impl_->storage.count = 0;
    impl_->storage.first_frame = impl_->storage.next_frame = 0;
}

std::vector<PlotPoint> DataStore::query(std::size_t channel, double tmin, double tmax,
                                      std::size_t pixel_width) const {
    const std::lock_guard lock(impl_->mutex);
    return impl_->storage.query(channel, tmin, tmax, pixel_width);
}

FrozenData DataStore::snapshot(double tmin, double tmax) const {
    std::vector<float> copied;
    std::uint64_t first = 0;
    {
        const std::lock_guard lock(impl_->mutex);
        const auto bounds = impl_->storage.interval(tmin, tmax);
        first = bounds.first;
        const auto count = static_cast<std::size_t>(bounds.second - bounds.first);
        const auto channel_count = channels().size();
        copied.resize(count * channel_count);
        if (count) {
            const auto slot = static_cast<std::size_t>(first % capacity_frames());
            const auto first_part = std::min(count, capacity_frames() - slot);
            std::copy_n(impl_->storage.values.begin() + slot * channel_count,
                        first_part * channel_count, copied.begin());
            std::copy_n(impl_->storage.values.begin(), (count - first_part) * channel_count,
                        copied.begin() + first_part * channel_count);
        }
    }
    // Allocate/build the independent summary pyramid after releasing the
    // producer lock. Only the chronological sample copy is synchronized.
    auto storage = std::make_shared<detail::SampleStorage>(
        sample_rate(), std::max(std::size_t{1}, copied.size() / channels().size()), channels());
    storage->append(first, copied);
    FrozenData result;
    result.storage_ = std::move(storage);
    return result;
}

const std::vector<ChannelInfo>& FrozenData::channels() const {
    static const std::vector<ChannelInfo> empty;
    return storage_ ? storage_->channels : empty;
}
double FrozenData::sample_rate() const { return storage_ ? storage_->sample_rate : 0; }
DataRange FrozenData::range() const { return storage_ ? storage_->range() : DataRange{}; }
std::vector<PlotPoint> FrozenData::query(std::size_t channel, double tmin, double tmax,
                                      std::size_t pixel_width) const {
    return storage_ ? storage_->query(channel, tmin, tmax, pixel_width) : std::vector<PlotPoint>{};
}

struct SimulationSource::Impl {
    DataStore& store;
    std::jthread worker;
    std::atomic<bool> is_running{false};
    std::atomic<std::uint64_t> produced{0};
    std::atomic<std::uint64_t> skipped{0};
    std::atomic<double> elapsed{0};

    explicit Impl(DataStore& target) : store(target) {}

    static float signal(std::size_t channel, std::uint64_t frame, double rate) {
        constexpr double pi = std::numbers::pi;
        const double t = static_cast<double>(frame) / rate;
        const double phase = 2 * pi * 50 * t;
        switch (channel % 8) {
        case 0: return static_cast<float>(8 * std::sin(phase));
        case 1: return static_cast<float>(8 * std::sin(phase - 2 * pi / 3));
        case 2: return static_cast<float>(8 * std::sin(phase + 2 * pi / 3));
        case 3: return std::fmod(t * 1000, 1.0) < 0.35 ? 6.0f : -6.0f;
        case 4: return static_cast<float>(5 * (1 - 4 * std::abs(std::fmod(t * 15, 1.0) - 0.5)));
        case 5: return static_cast<float>((3 + 2 * std::sin(2 * pi * 0.4 * t)) * std::sin(phase));
        case 6: {
            const auto period = std::max(std::uint64_t{4}, static_cast<std::uint64_t>(rate));
            const auto position = frame % period;
            const double impulse = position == period / 2 ? 18.0 :
                                   position == period / 2 + 1 ? -18.0 : 0.0;
            return static_cast<float>(3 * std::sin(phase) + impulse);
        }
        default: {
            // Deterministic integer hashing: no shared RNG state or allocation.
            auto bits = frame + 0x9e3779b97f4a7c15ULL;
            bits = (bits ^ (bits >> 30)) * 0xbf58476d1ce4e5b9ULL;
            bits = (bits ^ (bits >> 27)) * 0x94d049bb133111ebULL;
            bits ^= bits >> 31;
            const double noise = static_cast<double>(bits & 0xFFFF) / 32767.5 - 1;
            return static_cast<float>(0.55 * noise + 0.7 * std::sin(2 * pi * 850 * t));
        }
        }
    }

    void run(std::stop_token stop) {
        using clock = std::chrono::steady_clock;
        const auto initial_range = store.range();
        const auto base = initial_range.empty ? 0 : initial_range.last_frame + 1;
        const auto channel_count = store.channels().size();
        const double rate = store.sample_rate();
        const auto max_catchup = std::max(std::uint64_t{1}, static_cast<std::uint64_t>(rate * 0.1));
        std::vector<float> batch(static_cast<std::size_t>(max_catchup) * channel_count);
        const auto start = clock::now();
        auto wake = start;
        std::uint64_t generated = 0;
        while (!stop.stop_requested()) {
            const double seconds = std::chrono::duration<double>(clock::now() - start).count();
            elapsed.store(seconds, std::memory_order_relaxed);
            const auto target = static_cast<std::uint64_t>(seconds * rate);
            if (target > generated) {
                if (target - generated > max_catchup) {
                    const auto missed = target - generated - max_catchup;
                    skipped.fetch_add(missed, std::memory_order_relaxed);
                    generated += missed;
                }
                const auto frames = static_cast<std::size_t>(target - generated);
                for (std::size_t i = 0; i < frames; ++i)
                    for (std::size_t ch = 0; ch < channel_count; ++ch)
                        batch[i * channel_count + ch] = signal(ch, base + generated + i, rate);
                store.append(base + generated, std::span<const float>(batch.data(), frames * channel_count));
                produced.fetch_add(frames, std::memory_order_relaxed);
                generated = target;
            }
            wake += std::chrono::milliseconds(5);
            // After an OS pause, schedule forward rather than busy-looping
            // through old wake times. Missing samples are counted explicitly.
            const auto now = clock::now();
            if (wake < now) wake = now + std::chrono::milliseconds(5);
            std::this_thread::sleep_until(wake);
        }
        elapsed.store(std::chrono::duration<double>(clock::now() - start).count(), std::memory_order_relaxed);
        is_running.store(false, std::memory_order_release);
    }
};

SimulationSource::SimulationSource(DataStore& store) : impl_(std::make_unique<Impl>(store)) {}
SimulationSource::~SimulationSource() { stop(); }
void SimulationSource::start() {
    if (impl_->worker.joinable()) return;
    impl_->produced = 0;
    impl_->skipped = 0;
    impl_->elapsed = 0;
    impl_->is_running.store(true, std::memory_order_release);
    try {
        impl_->worker = std::jthread([this](std::stop_token stop) { impl_->run(stop); });
    } catch (...) {
        impl_->is_running.store(false, std::memory_order_release);
        throw;
    }
}
void SimulationSource::stop() {
    if (impl_->worker.joinable()) {
        impl_->worker.request_stop();
        impl_->worker.join();
    }
    impl_->is_running.store(false, std::memory_order_release);
}
bool SimulationSource::running() const { return impl_->is_running.load(std::memory_order_acquire); }
SourceStats SimulationSource::stats() const {
    return {impl_->produced.load(std::memory_order_relaxed),
            impl_->skipped.load(std::memory_order_relaxed),
            impl_->elapsed.load(std::memory_order_relaxed)};
}

} // namespace wave
