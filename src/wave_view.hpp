#pragma once
#include "data_core.hpp"
#include <memory>
#include <optional>

namespace wave {
class SignalHub;
// Optional demo theme. The host opts in to replacing its global style.
void apply_style(float dpi_scale = 1.0f);

struct ViewOptions {
    double time_span = 10.0;
    // Omitting both bounds enables automatic Y. Explicit bounds select a fixed scale.
    std::optional<double> y_min{};
    std::optional<double> y_max{};
    bool zoom_enabled = true;
    bool pan_enabled = true;
    bool pause_on_double_click = true;
};

// Two-finger translation since the previous frame, in logical pixels.
// Positive components mean physical finger motion right and up, respectively.
struct TouchpadDelta { float right = 0, up = 0; };

// One live plot in the current ImGui window/child's available area.
// The time axis follows live data. Pause affects only this view; the data store
// keeps receiving, and resuming jumps straight to the latest cached data.
// The host owns contexts, windows, layout, event loop and transport. Data must
// outlive the view. All view calls are UI-thread only; append may run elsewhere.
class WaveView {
public:
    explicit WaveView(DataStore& store, ViewOptions options = {});
    explicit WaveView(SignalHub& store, ViewOptions options = {});
    ~WaveView();
    WaveView(const WaveView&) = delete;
    WaveView& operator=(const WaveView&) = delete;

    void draw();
    // Pass the host's two-finger displacement once per frame. Consume its native
    // event to avoid duplicate wheel input. A zero delta is equivalent to draw().
    void draw(TouchpadDelta fingers);
    void set_time_span(double seconds);
    void set_y_range(double minimum, double maximum);
    void set_auto_y(bool enabled);
    void set_zoom_enabled(bool enabled);
    void set_pan_enabled(bool enabled);
    void set_pause_on_double_click(bool enabled);
    void set_paused(bool paused);
    bool paused() const;
    double time_span() const;
    // All channels, including newly registered ones, are visible by default.
    // Visibility changes only through this API, never through plot input.
    void set_channel_visible(std::size_t channel, bool visible);
    bool channel_visible(std::size_t channel) const;
    std::size_t points_this_frame() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wave
