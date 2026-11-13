#include "simulation_source.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <numbers>
#include <thread>

namespace wave {
std::vector<ChannelInfo> simulation_channels() {
    return {{"Phase A", "A"}, {"Phase B", "A"}, {"Phase C", "A"},
            {"PWM monitor", "V"}, {"Triangle", "V"}, {"Modulated", "A"},
            {"Single-sample spikes", "A"}, {"Noise + ripple", "V"}};
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
