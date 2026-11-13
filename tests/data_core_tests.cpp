#include "data_core.hpp"
#include "simulation_source.hpp"

#include <algorithm>
#include <atomic>
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
            "A gap larger than capacity must expire old history");
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
            next += 91; // Missing slots remain explicit within summary blocks.
            reference.insert(reference.end(), 91 * channels, std::numeric_limits<float>::quiet_NaN());
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
                require(!points.empty() && points.size() <= 4 * width + 3,
                        "Random range query must produce a bounded nonempty result");
                for (std::size_t i = 0; i < points.size(); ++i) {
                    const auto index = static_cast<std::uint64_t>(std::llround(points[i].t * rate));
                    require(index >= reference_first + left && index < reference_first + right,
                            "Random query cannot leak samples outside the requested range");
                    const float expected = reference[(index - reference_first) * channels + channel];
                    require(points[i].value == expected || (std::isnan(points[i].value) && std::isnan(expected)),
                            "Random query must match raw history at every returned timestamp");
                    if (i) require(points[i - 1].t < points[i].t, "Random output must be strictly ordered");
                }
                if (size <= width * 4) {
                    std::size_t valid=0;
                    for(auto i=left;i<right;++i) if(std::isfinite(reference[i*channels+channel])) ++valid;
                    require(points.size() == valid, "A narrow view must return all real samples and omit missing slots");
                    continue;
                }
                // Independent raw-array oracle checks the important property:
                // both extrema and endpoints of every display bin survive.
                for (std::size_t bin = 0; bin < width; ++bin) {
                    const auto lo = left + size * bin / width;
                    const auto hi = left + size * (bin + 1) / width;
                    float minimum = std::numeric_limits<float>::infinity();
                    float maximum = -minimum;
                    for (auto i = lo; i < hi; ++i) {
                        if (!std::isfinite(reference[i * channels + channel])) continue;
                        minimum = std::min(minimum, reference[i * channels + channel]);
                        maximum = std::max(maximum, reference[i * channels + channel]);
                    }
                    bool found_min = !std::isfinite(minimum), found_max = !std::isfinite(maximum),
                         found_first = !std::isfinite(reference[lo*channels+channel]),
                         found_last = !std::isfinite(reference[(hi-1)*channels+channel]);
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

void resize_retains_latest_and_snapshot() {
    wave::DataStore store(1000, 0.064, {{"A", "V"}, {"B", "A"}});
    std::vector<float> data;
    for (int i = 0; i < 157; ++i) { data.push_back(float(i)); data.push_back(float(-i)); }
    store.append(0, data);
    const auto frozen = store.snapshot(0, 1);
    const auto* metadata = &store.channels();
    store.set_capacity_frames(32);
    require(store.capacity_frames() == 32 && store.range().first_frame == 125,
            "Shrink must retain the latest complete frames after wraparound");
    for (std::size_t channel = 0; channel < 2; ++channel) {
        const auto points = store.query(channel, 0, 1, 100);
        require(points.size() == 32, "Both channels must have the resized capacity");
        for (std::size_t i = 0; i < points.size(); ++i)
            require(points[i].value == (channel ? -1 : 1) * float(125 + i),
                    "Resize must preserve sample order and channel alignment");
        const auto summary = store.query(channel, 0, 1, 1);
        require(summary.front().t == points.front().t && summary.back().t == points.back().t,
                "Rebuilt summaries must retain endpoints");
    }
    store.set_capacity_frames(128);
    require(store.range().frame_count == 32 && store.range().first_frame == 125,
            "Growing must preserve available history without inventing samples");
    store.append(157, std::vector<float>{157, -157, 158, -158});
    require(store.range().last_frame == 158 && store.range().frame_count == 34,
            "Append must continue the timeline after resizing");
    require(&store.channels() == metadata && frozen.range().frame_count == 64 &&
            frozen.query(0, 0, 1, 100).front().value == 93,
            "Metadata and frozen snapshots must remain stable");
    bool rejected = false;
    try { store.set_capacity_frames(0); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && store.capacity_frames() == 128 && store.range().frame_count == 34,
            "Invalid resize must leave the buffer intact");
    store.clear();
    store.set_capacity_frames(16);
    require(store.range().empty, "Resizing an empty buffer must keep it empty");
}

void source_lifecycle_and_concurrent_queries() {
    wave::DataStore store(20000, 0.2, wave::simulation_channels());
    wave::SimulationSource source(store);
    source.start();
    source.start(); // Idempotent: must not spawn a second producer.
    store.set_capacity_frames(3000);
    store.set_capacity_frames(5000);
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
void connected_view_queries() {
    const auto expect_times = [](const auto& points, const std::vector<double>& times) {
        require(points.size() == times.size(), "Connected query must retain the expected real samples");
        for (std::size_t i = 0; i < times.size(); ++i)
            require(std::abs(points[i].t - times[i]) < 1e-12 && std::isfinite(points[i].value),
                    "Connected query must preserve real sample times and values");
    };
    wave::DataStore uniform(10, 2, {{"A", "V"}});
    std::vector<float> samples(20);
    for (std::size_t i = 0; i < samples.size(); ++i) samples[i] = static_cast<float>(i);
    uniform.append(0, samples);
    expect_times(uniform.query(0, .35, .65, 100), {.4, .5, .6});
    expect_times(uniform.query(0, .35, .65, 100, 0, true), {.3, .4, .5, .6, .7});
    require(uniform.query(0, .35, .36, 100).empty(), "Exact query must keep its existing interval contract");
    expect_times(uniform.query(0, .35, .36, 100, 0, true), {.3, .4});
    expect_times(uniform.query(0, .35, .36, 100, 4, true), {.4});
    require(uniform.query(0, .35, .36, 100, 5, true).empty(), "Neighbor query must respect the clear cutoff");
    for (auto width : {1u, 1000u}) {
        const auto points = uniform.query(0, .25, 1.55, width, 0, true);
        require(std::abs(points.front().t - .2) < 1e-12 && std::abs(points.back().t - 1.6) < 1e-12,
                "Raw and reduced queries must preserve both outside neighbors");
    }
    require(uniform.query(0, -2, -1, 100, 0, true).empty() &&
            uniform.query(0, 2, 3, 100, 0, true).empty(), "Neighbors must stay within retained history");
    const auto frozen = uniform.snapshot(0, 2);
    uniform.clear_channel(0);
    require(uniform.query(0, .35, .36, 100, 0, true).empty(), "Neighbors must not resurrect cleared samples");
    require(frozen.query(0, .35, .36, 100, uniform.channel_first_frame(0), true).empty(),
            "Live clear cutoff must also filter frozen neighbors");
    uniform.set_capacity_frames(8);
    expect_times(frozen.query(0, .35, .36, 100, 0, true), {.3, .4});

    wave::DataStore gaps(1, 32, {{"A", "V"}});
    gaps.append(0, std::vector<float>{0});
    gaps.append(20, std::vector<float>{20});
    for (auto width : {1u, 1000u})
        expect_times(gaps.query(0, 5.2, 6.8, width, 0, true), {0, 20});
    expect_times(gaps.query(0, 5.2, 6.8, 100, 1, true), {20});

    wave::DataStore timed(1, 8, {});
    timed.configure(0, {{"A", "V"}}, true);
    const std::vector<double> times{0, .37, 1.2, 1.5};
    for (std::size_t i = 0; i < times.size(); ++i) {
        const float value = static_cast<float>(i);
        timed.append_received(i, std::span<const float>(&value, 1), times[i]);
    }
    require(timed.query(0, .4, .5, 100).empty(), "Exact irregular query must stay empty between samples");
    expect_times(timed.query(0, .4, .5, 100, 0, true), {.37, 1.2});
    expect_times(timed.query(0, .37, 1.2, 100, 0, true), times);
    expect_times(timed.snapshot(0, 2).query(0, .4, .5, 100, 0, true), {.37, 1.2});

    timed.clear();
    timed.set_capacity_frames(256);
    std::uint64_t frame = 0;
    const auto receive = [&](double t, float value) {
        timed.append_received(frame++, std::span<const float>(&value, 1), t);
    };
    receive(0, 0);
    receive(.25, 0);
    receive(.6, 0);
    for (int i = 0; i < 100; ++i)
        receive(1.0 + i * 1e-5, i == 20 ? 4.0f : i == 30 ? -5.0f : 0.0f);
    receive(1.45, 0);
    receive(1.7, 0);
    const auto reduced = timed.query(0, .2, 1.5, 16, 0, true);
    expect_times(reduced, {0, .25, .6, 1.0, 1.0002, 1.0003, 1.00099, 1.45, 1.7});
    require(reduced[4].value == 4 && reduced[5].value == -5,
            "Reducing a burst must keep both extrema together with isolated readings and outside neighbors");
    std::cout << "PASS viewport neighbors: raw, reduced, gaps, cutoffs, frozen and receiver timestamps\n";
}
} // namespace

int main() {
    try {
        connected_view_queries();
        {
            wave::DataStore gaps(1000, .128, {{"A", "V"}});
            gaps.append(0, std::vector<float>(40, 2));
            gaps.append(42, std::vector<float>(40, 7));
            require(gaps.range().first_frame == 0 && gaps.range().last_frame == 81,
                    "Small missing batch must not clear existing data");
            for (auto width : {1u, 1000u}) {
                const auto points = gaps.query(0, 0, 1, width);
                bool before=false, hole=false, after=false;
                for (const auto& p:points) {
                    if (p.t < .04 && p.value==2) before=true;
                    if (std::isnan(p.value)) hole=true;
                    if (p.t >= .042 && p.value==7) after=true;
                }
                require(before && !hole && after, "Raw and reduced queries must connect real data across missing slots");
                bool connected=false;
                for(std::size_t i=1;i<points.size();++i)
                    if(std::abs(points[i-1].t-.039)<1e-12 && std::abs(points[i].t-.042)<1e-12)
                        connected=true;
                require(connected, "Gap endpoints must be adjacent for direct rendering");
            }
            const auto frozen=gaps.snapshot(0,1);
            gaps.set_capacity_frames(64);
            require(gaps.range().first_frame==18, "Resize must retain the latest timeline slots");
            const auto resized=gaps.query(0,0,1,1);
            require(std::all_of(resized.begin(),resized.end(),[](auto p){return std::isfinite(p.value);}) && resized.front().value==2 && resized.back().value==7,
                    "Resize must keep both sides connected without missing samples");
            gaps.append(82,std::vector<float>(200,9));
            require(gaps.query(0,0,1,1).size()<=4, "Evicted gaps must not leak into new queries");
            const auto old=frozen.query(0,0,1,1);
            require(std::all_of(old.begin(),old.end(),[](auto p){return std::isfinite(p.value);}) && old.front().value==2 && old.back().value==7,
                    "Frozen real history must stay connected after live rollover");
            std::cout << "PASS gaps retain history, connect across missing slots, survive resize/freeze and expire on rollover\n";
        }
        {
            wave::DataStore timed(1, 32, {});
            timed.configure(0, {{"Value", "", "value"}}, true);
            for (std::uint64_t i=0;i<80;++i) {
                const float value=float(i);
                timed.append_received(i, std::span<const float>(&value,1), double(i*i)/1000);
            }
            const auto range=timed.range();
            require(range.first_frame==48 && range.begin==2.304 && range.end==6.241, "Irregular ring must retain actual times");
            auto snapshot=timed.snapshot(0,10);
            const auto points=timed.query(0,3.0,4.0,100);
            require(points.size()==9 && points.front().t==3.025 && points.back().t==3.969, "Irregular queries must binary-search actual times");
            const auto reduced=timed.query(0,0,10,1);
            require(reduced.front().value==48 && reduced.back().value==79, "Irregular summaries must preserve extrema");
            timed.set_capacity_frames(8);
            require(timed.range().begin==5.184 && timed.query(0,0,10,100).size()==8, "Resize must preserve timestamps");
            timed.clear_channel(0);
            require(timed.query(0,0,10,100).empty() && snapshot.query(0,0,10,100).size()==32, "Clear must use indices; snapshot remains immutable");
            require(snapshot.query(0,0,10,100,timed.channel_first_frame(0)).empty(), "Live clear cutoff must also filter frozen history");
            const float value=80;
            timed.append_received(80,std::span<const float>(&value,1),7.123);
            require(timed.query(0,0,10,100).front().t==7.123, "New point must survive channel clear");
            bool rejected=false;
            try {timed.append_received(81,std::span<const float>(&value,1),7.0);} catch(const std::invalid_argument&) {rejected=true;}
            require(rejected && timed.range().last_frame==80, "Backwards timestamp must be rejected atomically");
            std::cout << "PASS receiver timestamps: irregular query, rollover, summary, resize, clear and snapshot\n";
        }
        rollover_and_snapshot();
        extrema_and_ordering();
        summaries_after_many_wraps();
        invalid_input_is_atomic();
        randomized_summary_reference();
        resize_retains_latest_and_snapshot();
        source_lifecycle_and_concurrent_queries();
        std::cout << "PASS: rollover, immutable pause, peak preservation, summary wrap, validation, source lifecycle\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
