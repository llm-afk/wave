#include "data_core.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void rollover_and_snapshot() {
    wave::DataStore store(1000, 0.020, {{"A", "V"}, {"B", "A"}});
    std::vector<float> input;
    for (int i = 0; i < 47; ++i) {
        input.push_back(static_cast<float>(i));
        input.push_back(static_cast<float>(-i));
    }
    store.append(0, input);
    auto range = store.range();
    require(range.first_frame == 27 && range.last_frame == 46 && range.frame_count == 20,
            "Ring rollover must retain the most recent whole frames");
    auto points = store.query(1, 0.025, 0.032, 100);
    require(points.size() == 6 && points.front().value == -27 && points.back().value == -32,
            "Queries must clip old data and preserve channel interleaving");
    require(store.query(0, 0, 0.02, 100).empty(), "Overwritten ranges must be empty");
    require(store.query(0, 0.0312, 0.0318, 100).empty(), "Sub-sample interval has no samples");

    auto frozen = store.snapshot(0.03, 0.04);
    std::vector<float> replacement(80, 999);
    store.append(47, replacement);
    const auto paused = frozen.query(0, 0, 1, 100);
    require(paused.size() == 11 && paused.front().value == 30 && paused.back().value == 40,
            "A frozen snapshot must survive source ring overwrite");
    require(frozen.range().first_frame == 30, "Snapshot must retain source timestamps");
}

void extrema_and_ordering() {
    wave::DataStore store(20000, 2, {{"Signal", "A"}});
    std::vector<float> values(40000, 1);
    values[177] = 200;
    values[178] = -190;
    values[32768] = 150;
    store.append(0, values);
    const auto points = store.query(0, 0, 2, 41);
    require(points.size() <= 164, "Display query must be bounded by pixel count");
    require(points.front().t == 0 && points.back().t == 39999.0 / 20000,
            "First and last points must survive downsampling");
    require(std::any_of(points.begin(), points.end(), [](const auto& p) { return p.value == 200; }),
            "Single-sample positive spike must survive zoom-out");
    require(std::any_of(points.begin(), points.end(), [](const auto& p) { return p.value == -190; }),
            "Adjacent opposite spike must survive zoom-out");
    for (std::size_t i = 1; i < points.size(); ++i)
        require(points[i - 1].t < points[i].t, "Extrema must remain in strict time order");
    const auto zoomed = store.query(0, 175.0 / 20000, 180.0 / 20000, 1000);
    require(zoomed.size() == 6 && zoomed[2].value == 200 && zoomed[3].value == -190,
            "Zoom-in must recover original neighboring samples");
}

void summaries_after_many_wraps() {
    wave::DataStore store(1000, 1.003, {{"Signal", "V"}});
    constexpr std::size_t total = 11017;
    std::vector<float> values(total);
    for (std::size_t i = 0; i < total; ++i)
        values[i] = static_cast<float>((i * 7919) % 997) - 400;
    store.append(0, values);
    const auto available = store.range();
    for (const std::size_t width : {std::size_t{1}, std::size_t{7}, std::size_t{23}}) {
        const auto output = store.query(0, available.begin, available.end, width);
        const auto retained = values.begin() + available.first_frame;
        const auto expected = std::minmax_element(retained, values.end());
        const auto actual = std::minmax_element(output.begin(), output.end(),
            [](const auto& a, const auto& b) { return a.value < b.value; });
        require(actual.first->value == *expected.first && actual.second->value == *expected.second,
                "Summary pyramid must remain correct after unaligned ring wraps");
        for (const auto& p : output) {
            const auto frame = static_cast<std::size_t>(std::llround(p.t * 1000));
            require(frame >= available.first_frame && frame < total && p.value == values[frame],
                    "Summaries must not emit overwritten samples");
        }
    }
    std::vector<float> newer(800, -2);
    newer[101] = 55;
    store.append(30007, newer);
    const auto discontinuous = store.query(0, 0, 100, 3);
    require(store.range().first_frame == 30007 &&
            std::any_of(discontinuous.begin(), discontinuous.end(), [](const auto& p) { return p.value == 55; }),
            "A discontinuity must reset stale summary history");
}

void invalid_input_is_atomic() {
    wave::DataStore store(1000, 1, {{"A", "V"}});
    const std::vector<float> initial{1, 2, 3};
    store.append(0, initial);
    bool rejected = false;
    try {
        const std::vector<float> bad{4, std::numeric_limits<float>::quiet_NaN(), 6};
        store.append(3, bad);
    } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && store.range().last_frame == 2,
            "An invalid batch must not partially mutate the history");
}

void randomized_summary_reference() {
    constexpr std::size_t capacity = 4099;
    constexpr std::size_t channels = 3;
    constexpr double rate = 20000;
    wave::DataStore store(rate, static_cast<double>(capacity) / rate,
                          {{"A", "V"}, {"B", "V"}, {"C", "V"}});
    require(store.capacity_frames() == capacity, "Fractional history duration must round to intended frames");
    std::uint32_t rng = 0x37a0b16d;
    const auto random = [&rng]() {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return rng;
    };
    std::vector<float> reference;
    std::uint64_t next = 13;
    for (std::size_t iteration = 0; iteration < 80; ++iteration) {
        if (iteration == 37) {
            next += 91; // A new segment begins within summary blocks.
            reference.clear();
        }
        const auto count = 17 + random() % 1700;
        std::vector<float> batch(count * channels);
        for (auto& value : batch) value = static_cast<float>(random() % 2001) - 1000;
        store.append(next, batch);
        next += count;
        reference.insert(reference.end(), batch.begin(), batch.end());
        if (reference.size() > capacity * channels)
            reference.erase(reference.begin(), reference.end() - capacity * channels);
        const auto retained = reference.size() / channels;
        const auto reference_first = next - retained;
        require(store.range().first_frame == reference_first, "Reference history and ring must agree");
        const auto left = static_cast<std::size_t>(random() % (retained / 3 + 1));
        const auto right = retained - static_cast<std::size_t>(random() % (retained / 3 + 1));
        for (std::size_t channel = 0; channel < channels; ++channel) {
            for (const std::size_t width : {std::size_t{1}, std::size_t{7}, std::size_t{53}}) {
                const auto points = store.query(channel, (reference_first + left) / rate,
                                                 (reference_first + right - 1) / rate, width);
                const auto size = right - left;
                require(!points.empty() && points.size() <= 4 * width,
                        "Random range query must produce a bounded nonempty result");
                for (std::size_t i = 0; i < points.size(); ++i) {
                    const auto index = static_cast<std::uint64_t>(std::llround(points[i].t * rate));
                    require(index >= reference_first + left && index < reference_first + right,
                            "Random query cannot leak samples outside the requested range");
                    require(points[i].value == reference[(index - reference_first) * channels + channel],
                            "Random query must match raw history at every returned timestamp");
                    if (i) require(points[i - 1].t < points[i].t, "Random output must be strictly ordered");
                }
                if (size <= width * 4) {
                    require(points.size() == size, "A narrow view must return every original sample");
                    continue;
                }
                // Independent raw-array oracle checks the important property:
                // both extrema and endpoints of every display bin survive.
                for (std::size_t bin = 0; bin < width; ++bin) {
                    const auto lo = left + size * bin / width;
                    const auto hi = left + size * (bin + 1) / width;
                    float minimum = reference[lo * channels + channel];
                    float maximum = minimum;
                    for (auto i = lo; i < hi; ++i) {
                        minimum = std::min(minimum, reference[i * channels + channel]);
                        maximum = std::max(maximum, reference[i * channels + channel]);
                    }
                    bool found_min = false, found_max = false, found_first = false, found_last = false;
                    for (const auto& point : points) {
                        const auto index = static_cast<std::uint64_t>(std::llround(point.t * rate));
                        if (index >= reference_first + lo && index < reference_first + hi) {
                            found_min |= point.value == minimum;
                            found_max |= point.value == maximum;
                            found_first |= index == reference_first + lo;
                            found_last |= index == reference_first + hi - 1;
                        }
                    }
                    require(found_min && found_max && found_first && found_last,
                            "Random summary query must preserve each bin's extrema and endpoints");
                }
            }
        }
    }
}

void source_lifecycle_and_concurrent_queries() {
    wave::DataStore store(20000, 0.2);
    wave::SimulationSource source(store);
    source.start();
    source.start(); // Idempotent: must not spawn a second producer.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (source.stats().produced_frames < 2000 && std::chrono::steady_clock::now() < deadline) {
        const auto range = store.range();
        if (!range.empty) {
            const auto points = store.query(6, range.begin, range.end, 100);
            for (const auto& point : points)
                require(std::isfinite(point.t) && std::isfinite(point.value),
                        "Concurrent display reads must see finite committed samples");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    source.stop();
    const auto stats = source.stats();
    require(!source.running() && stats.produced_frames >= 2000,
            "Source must produce paced data and stop cleanly");
    const auto stopped_frame = store.range().last_frame;
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    require(store.range().last_frame == stopped_frame, "No frames may arrive after stop returns");
    source.start();
    const auto restart_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (source.stats().produced_frames == 0 && std::chrono::steady_clock::now() < restart_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    source.stop();
    require(store.range().last_frame > stopped_frame, "Restart must resume the source timeline");
}
} // namespace

int main() {
    try {
        rollover_and_snapshot();
        extrema_and_ordering();
        summaries_after_many_wraps();
        invalid_input_is_atomic();
        randomized_summary_reference();
        source_lifecycle_and_concurrent_queries();
        std::cout << "PASS: rollover, immutable pause, peak preservation, summary wrap, validation, source lifecycle\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
