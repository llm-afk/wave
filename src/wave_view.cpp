#include "wave_view.hpp"
#include "plot_navigation.hpp"
#include "signal_hub.hpp"
#include "imgui.h"
#include "implot.h"
#include "implot_internal.h" // Pinned-version legend bounds for gesture hit testing.
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace wave {
namespace {
constexpr std::array<ImVec4, 8> signal_colors = {{
    {1.00f, 0.267f, 0.267f, 1}, {0.267f, 1.00f, 0.267f, 1},
    {0.267f, 0.533f, 1.00f, 1}, {1.00f, 0.82f, 0.267f, 1},
    {0.80f, 0.40f, 1.00f, 1}, {0.267f, 0.90f, 1.00f, 1},
    {1.00f, 0.60f, 0.267f, 1}, {0.77f, 0.81f, 0.86f, 1}
}};
constexpr float line_alpha = 0.60f;
constexpr float full_marker_alpha = 0.95f;
constexpr double hidden_spacing_diameters = 0.75;
constexpr double resolved_spacing_diameters = 3.0;
constexpr double auto_y_fill = 0.80;

float marker_visibility(double interval, double seconds_per_diameter) {
    const double spacing = interval / seconds_per_diameter;
    const float t = static_cast<float>(std::clamp((spacing - hidden_spacing_diameters) /
        (resolved_spacing_diameters - hidden_spacing_diameters), 0.0, 1.0));
    return t * t * (3.0f - 2.0f * t);
}

void draw_sample_markers(std::span<const PlotPoint> samples, double rate, double seconds_per_pixel,
                         float line_width, ImVec4 color) {
    const float radius = 0.7f * std::max(1.0f, 1.5f * line_width);
    const double seconds_per_diameter = seconds_per_pixel * (2.0 * radius);
    const bool uniform = rate > 0 && samples.size() > 1;
    const float uniform_detail = uniform ? marker_visibility(1.0 / rate, seconds_per_diameter) : 1.0f;
    if (uniform_detail == 0) return;

    auto* draw = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();
    ImRect bounds(draw->GetClipRectMin(), draw->GetClipRectMax());
    bounds.Expand(radius + 1.0f);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const auto& p = samples[i];
        float detail = uniform_detail;
        if (rate == 0 && samples.size() > 1) {
            const auto infinity = std::numeric_limits<double>::infinity();
            const double before = i > 0 ? p.t - samples[i - 1].t : infinity;
            const double after = i + 1 < samples.size() ? samples[i + 1].t - p.t : infinity;
            detail = marker_visibility(std::min(before, after), seconds_per_diameter);
        }
        if (detail == 0) continue;
        const auto xy = ImPlot::PlotToPixels(p.t, p.value);
        if (!bounds.Contains(xy)) continue;
        color.w = full_marker_alpha * detail;
        const float marker_radius = std::lerp(std::max(0.5f, line_width * 0.5f), radius, detail);
        draw->AddCircleFilled(xy, marker_radius, ImGui::GetColorU32(color));
    }
    ImPlot::PopPlotClipRect();
}

void validate_span(double seconds) {
    if (!std::isfinite(seconds) || seconds <= 0)
        throw std::invalid_argument("Time span must be finite and positive");
}
void validate_y(double minimum, double maximum) {
    if (!std::isfinite(minimum) || !std::isfinite(maximum) ||
        minimum >= maximum || !std::isfinite(maximum - minimum))
        throw std::invalid_argument("Y limits must define a finite, positive range");
}
// Include the clipped ends of the visible polyline without fitting to a peak outside the view.
void extend_visible_y(AxisRange& y, std::span<const PlotPoint> samples, AxisRange x) {
    const auto include = [&](double value) {
        y.min = std::min(y.min, value);
        y.max = std::max(y.max, value);
    };
    for (const auto& p : samples)
        if (p.t >= x.min && p.t <= x.max && std::isfinite(p.value)) include(p.value);
    if (samples.size() < 2) return;
    const auto include_edge = [&](const PlotPoint& a, const PlotPoint& b, double edge) {
        if (a.t < edge && edge < b.t && std::isfinite(a.value) && std::isfinite(b.value))
            include(std::lerp(a.value, b.value, (edge - a.t) / (b.t - a.t)));
    };
    include_edge(samples.front(), samples[1], x.min);
    include_edge(samples[samples.size() - 2], samples.back(), x.max);
}
bool contains(ImVec2 point, ImVec2 position, ImVec2 size) {
    return point.x >= position.x && point.y >= position.y &&
        point.x < position.x + size.x && point.y < position.y + size.y;
}
} // namespace

void apply_style(float scale) {
    ImGui::GetStyle() = ImGuiStyle{};
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowPadding = {6, 5};
    style.WindowRounding = 0;
    style.ChildRounding = 0;
    style.WindowBorderSize = 0;
    style.ChildBorderSize = 0;
    style.Colors[ImGuiCol_Text] = {0.90f, 0.90f, 0.90f, 1};
    style.Colors[ImGuiCol_TextDisabled] = {0.57f, 0.57f, 0.57f, 1};
    style.Colors[ImGuiCol_WindowBg] = {0, 0, 0, 1};
    style.Colors[ImGuiCol_ChildBg] = {0, 0, 0, 1};
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    auto& plot = ImPlot::GetStyle();
    plot.PlotPadding = {5 * scale, 4 * scale};
    plot.LabelPadding = {3 * scale, 2 * scale};
    plot.LegendPadding = {7 * scale, 5 * scale};
    plot.LegendInnerPadding = {0, 0};
    plot.PlotBorderSize = 0;
    plot.Colors[ImPlotCol_FrameBg] = {0, 0, 0, 1};
    plot.Colors[ImPlotCol_PlotBg] = {0, 0, 0, 1};
    plot.Colors[ImPlotCol_AxisText] = {0.80f, 0.80f, 0.80f, 1};
    plot.Colors[ImPlotCol_AxisTick] = {0.56f, 0.56f, 0.56f, 1};
    plot.Colors[ImPlotCol_LegendBg] = {0, 0, 0, 0};
    plot.Colors[ImPlotCol_LegendBorder] = {0, 0, 0, 0};
    plot.Colors[ImPlotCol_LegendText] = {0.78f, 0.78f, 0.78f, 1};
}

struct WaveView::Impl {
    static constexpr double infinity = std::numeric_limits<double>::infinity();
    struct LiveTimeline {
        DataRange range;
        double playhead = -infinity;
        double capacity_seconds = infinity;
        double full_ring_seconds = infinity;
    };
    SignalHub owned_store;
    SignalHub& store;
    ViewOptions options;
    AxisRange y;
    bool auto_y = true, fitted_y = false;
    double shrink_since = -1;
    double capacity_seconds = infinity;
    bool automatic_span = true;
    std::vector<bool> visible;
    std::size_t points = 0;
    ImVec2 plot_pos{}, plot_size{}, frame_pos{}, frame_size{};
    ImVec2 legend_pos{}, legend_size{};
    SignalSnapshot frozen;
    bool paused = false, displayed = false;
    AxisRange paused_x{};
    PlotRange last_limits{};
    double last_drawn_until = infinity, frozen_until = infinity;
    double resume_floor = -std::numeric_limits<double>::infinity();
    struct Drag {
        bool held = false, moved = false;
        ImVec2 press{};
        PlotRange initial{};
        ZoomAnchor anchor{};
        ZoomAxes axes = ZoomAxes::Both;
        DragZoomLimits limits{};
    } drag;
    struct Pan {
        bool held = false, moved = false;
        ImVec2 press{}, size{};
        PlotRange initial{};
        ZoomAxes axes = ZoomAxes::Both;
    } pan;

    Impl(DataStore& data, ViewOptions config) : owned_store(data), store(owned_store), options(config) {
        init_axes();
    }
    Impl(SignalHub& data, ViewOptions config) : store(data), options(config) {
        init_axes();
    }
    void init_axes() {
        validate_span(options.time_span);
        y = {options.y_min.value_or(-1.0), options.y_max.value_or(1.0)};
        validate_y(y.min, y.max);
        auto_y = !options.y_min && !options.y_max;
    }

    void sync_channels() {
        // SignalHub already publishes metadata when sources are registered.
        if (store.legacy()) store.sync();
        visible.resize(store.channels().size(), true);
    }

    LiveTimeline live_timeline() const {
        LiveTimeline online, offline;
        for (std::size_t i = 0; i < visible.size(); ++i) {
            if (!visible[i]) continue;
            const auto r = store.channel_range(i, store.channel_first_frame(i));
            if (r.empty) continue;
            const auto& source = store.source(i);
            auto& result = source.online ? online : offline;
            result.range = merge_ranges(result.range, r);
            result.playhead = std::max(result.playhead, source.clock.position() + source.offset);
            const auto capacity = source.data->capacity_frames();
            const auto rate = source.data->sample_rate();
            double duration = infinity;
            if (rate > 0) duration = capacity / rate;
            else if (r.frame_count == capacity && r.end > r.begin) duration = r.end - r.begin;
            result.capacity_seconds = std::min(result.capacity_seconds, duration);
            if (r.frame_count == capacity)
                result.full_ring_seconds = std::min(result.full_ring_seconds, duration);
        }
        return online.range.empty ? offline : online;
    }

    double live_span() const {
        return automatic_span ? std::min(options.time_span, capacity_seconds) : options.time_span;
    }
    double display_time(const LiveTimeline& live) const {
        // A viewport using the entire ring has no spare history for a delayed playhead.
        if (live_span() >= live.capacity_seconds) return live.range.end;
        return std::clamp(std::max(live.playhead, resume_floor), live.range.begin, live.range.end);
    }
    AxisRange live_x(const LiveTimeline& live) const {
        const double span = live_span();
        if (live.range.empty) return {0, span};
        const double head = display_time(live);
        if (head - span >= live.range.begin || span >= live.full_ring_seconds)
            return {std::min(head - span, std::nextafter(head, -infinity)), head};
        const double left = live.range.begin;
        return {left, std::max(left + span, std::nextafter(left, infinity))};
    }

    void fit_y(AxisRange data, bool viewport_changed) {
        if (data.min > data.max) { shrink_since = -1; return; }
        const double span = data.max - data.min;
        const double padding = span > 0 ? span * (1.0 / auto_y_fill - 1.0) * 0.5
                                        : (data.min == 0 ? 1.0 : std::abs(data.min) * 0.05);
        const AxisRange target{data.min - padding, data.max + padding};
        const double current_span = y.max - y.min;
        const bool expand = data.min < y.min + 0.05 * current_span || data.max > y.max - 0.05 * current_span;
        if (fitted_y && !expand && !viewport_changed) {
            // Ignore small fluctuations and require a sustained change before contracting or recentering.
            const double tolerance = 0.025 * current_span;
            if (paused || (std::abs(target.min - y.min) <= tolerance && std::abs(target.max - y.max) <= tolerance)) {
                shrink_since = -1;
                return;
            }
            const double now = ImGui::GetTime();
            if (shrink_since < 0) shrink_since = now;
            if (now - shrink_since < 0.5) return;
        }
        y = target;
        fitted_y = true;
        shrink_since = -1;
    }

    void set_paused(bool value) {
        if (paused == value) return;
        sync_channels();
        if (value) {
            const auto available = store.range();
            if (available.empty) return;
            auto snapshot = store.snapshot(available.begin, available.end);
            if (snapshot.range().empty) return;
            frozen = std::move(snapshot);
            const auto live = live_timeline();
            capacity_seconds = live.capacity_seconds;
            paused_x = displayed && points != 0 ? last_limits.x : live_x(live);
            frozen_until = displayed && points != 0 && last_drawn_until < paused_x.max ? last_drawn_until : infinity;
        } else {
            frozen = {};
            const auto live = live_timeline().range;
            // Jump straight to the current cache. Do not rewind to the delayed
            // display playhead on the following frames while it catches up.
            resume_floor = live.empty ? -std::numeric_limits<double>::infinity() : live.end;
        }
        paused = value;
        drag = {};
        pan = {};
    }

    void navigate(PlotRange& range) {
        auto& io = ImGui::GetIO();
        if (io.AppFocusLost ||
            ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
            drag = {};
            pan = {};
            return;
        }
        const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
            contains(io.MousePos, frame_pos, frame_size);
        const bool legend = contains(io.MousePos, legend_pos, legend_size);
        const bool plot = hovered && !legend && contains(io.MousePos, plot_pos, plot_size);
        const bool x_axis = hovered && io.MousePos.x >= plot_pos.x &&
            io.MousePos.x < plot_pos.x + plot_size.x && io.MousePos.y >= plot_pos.y + plot_size.y;
        const bool y_axis = hovered && io.MousePos.x < plot_pos.x &&
            io.MousePos.y >= plot_pos.y && io.MousePos.y < plot_pos.y + plot_size.y;
        if (plot && options.pause_on_double_click && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            set_paused(!paused);
            return;
        }
        if (options.pan_enabled && !drag.held && (plot || x_axis || y_axis) &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            pan = {true, false, io.MousePos, plot_size, range,
                x_axis ? ZoomAxes::XOnly : y_axis ? ZoomAxes::YOnly : ZoomAxes::Both};
        }
        if (pan.held) {
            const double dx = io.MousePos.x - pan.press.x, dy = io.MousePos.y - pan.press.y;
            pan.moved |= dx * dx + dy * dy >= 16 * ImGui::GetStyle().FontScaleDpi * ImGui::GetStyle().FontScaleDpi;
            if (pan.moved) {
                if (pan.axes != ZoomAxes::XOnly && (dy != 0 || !auto_y)) {
                    const double shift = dy / std::max(1.0f, pan.size.y) * (pan.initial.y.max - pan.initial.y.min);
                    const AxisRange next{pan.initial.y.min + shift, pan.initial.y.max + shift};
                    if (std::isfinite(next.min) && std::isfinite(next.max) && next.min < next.max) {
                        y = range.y = next;
                        auto_y = false;
                    }
                }
                if (paused && pan.axes != ZoomAxes::YOnly) {
                    const double shift = -dx / std::max(1.0f, pan.size.x) * (pan.initial.x.max - pan.initial.x.min);
                    const AxisRange next{pan.initial.x.min + shift, pan.initial.x.max + shift};
                    if (std::isfinite(next.min) && std::isfinite(next.max) && next.min < next.max)
                        range.x = paused_x = next;
                }
            }
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) pan = {};
            return;
        }
        if (!options.zoom_enabled) { drag = {}; return; }
        if (!drag.held && (!(plot || x_axis || y_axis) ||
            (!ImGui::IsMouseClicked(ImGuiMouseButton_Right) && io.MouseWheel == 0))) return;
        const auto axes = x_axis ? ZoomAxes::XOnly : y_axis ? ZoomAxes::YOnly : ZoomAxes::Both;
        const double fraction = std::clamp(double(io.MousePos.y - plot_pos.y) / std::max(1.0f, plot_size.y), 0.0, 1.0);
        const double x_fraction = std::clamp(double(io.MousePos.x - plot_pos.x) / std::max(1.0f, plot_size.x), 0.0, 1.0);
        const ZoomAnchor anchor{paused ? range.x.min + x_fraction * (range.x.max - range.x.min) : range.x.max,
            range.y.max - fraction * (range.y.max - range.y.min)};
        const float dpi = ImGui::GetStyle().FontScaleDpi;
        DragZoomLimits limits;
        // Avoid imposing a new bound on host-provided ranges.
        const auto history = store.range();
        limits.max_x_span = std::max({3600.0, options.time_span, 2 * (history.end - history.begin)});
        limits.max_y_span = std::max(1e12, y.max - y.min);
        limits.pixels_per_e_fold = default_zoom_pixels_per_e_fold * dpi;
        const auto apply = [&](PlotRange next, ZoomAxes active_axes, bool move_x, bool move_y) {
            if (active_axes != ZoomAxes::YOnly && (move_x || !automatic_span)) {
                automatic_span = false;
                options.time_span = next.x.max - next.x.min;
                if (paused) paused_x = next.x;
            }
            if (active_axes != ZoomAxes::XOnly && (move_y || !auto_y)) {
                auto_y = false;
                y = range.y = next.y;
            }
        };
        if ((plot || x_axis || y_axis) && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            drag = {true, false, io.MousePos, range, anchor, axes, limits};
        if (drag.held) {
            const double dx = io.MousePos.x - drag.press.x, dy = io.MousePos.y - drag.press.y;
            const double threshold = 3 * dpi;
            drag.moved |= dx * dx + dy * dy >= threshold * threshold;
            if (drag.moved)
                apply(zoom_from_drag(drag.initial, drag.anchor, dx * 1.95, dy * 1.95, drag.axes, drag.limits),
                    drag.axes, dx != 0, dy != 0);
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) drag = {};
        } else if ((plot || x_axis || y_axis) && io.MouseWheel != 0) {
            const double delta = std::clamp(double(io.MouseWheel), -8.0, 8.0) * 35 * dpi;
            apply(zoom_from_drag(range, anchor, delta, -delta, axes, limits), axes, true, true);
        }
    }

    void draw() {
        sync_channels();
        store.update_clocks(ImGui::GetTime());
        const auto live = live_timeline();
        capacity_seconds = live.capacity_seconds;
        if (live.range.empty || live.range.end < resume_floor || live.playhead >= resume_floor)
            resume_floor = -infinity;
        PlotRange limits{paused ? paused_x : live_x(live), y};
        navigate(limits);
        limits.x = paused ? paused_x : live_x(live);
        const double left = limits.x.min;
        const double until = std::min(limits.x.max, paused ? frozen_until : display_time(live));
        points = 0;

        // Local IDs allow independent instances in the same host window.
        ImGui::PushID(this);
        const auto available = ImGui::GetContentRegionAvail();
        const auto minimum = ImPlot::GetStyle().PlotMinSize;
        const ImVec2 plot_area{std::max(minimum.x, available.x - 1), std::max(minimum.y, available.y - 1)};
        if (options.zoom_enabled || options.pan_enabled || options.pause_on_double_click) {
            // Register an input target before the plot uses the same rectangle.
            // This reserves wheel input locally without changing host layout.
            const auto cursor = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##plot_input", plot_area,
                ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
            if (options.zoom_enabled) ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
            ImGui::SetCursorScreenPos(cursor);
        }
        constexpr auto flags = ImPlotFlags_NoTitle | ImPlotFlags_NoMenus |
            ImPlotFlags_NoBoxSelect | ImPlotFlags_NoMouseText | ImPlotFlags_NoInputs;
        if (ImPlot::BeginPlot("##wave", plot_area, flags)) {
            const auto read_trace = [&](std::size_t i, std::size_t width) {
                const double fraction = std::clamp((until - left) / (limits.x.max - left), 0.0, 1.0);
                width = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(width * fraction)));
                auto samples = paused
                    ? frozen.query(i, left, until, width, store.channel_first_frame(i), true)
                    : store.query(i, left, until, width, 0, true);
                // A trailing interpolation neighbor must not leak ahead into the unfilled area.
                if (until < limits.x.max && !samples.empty() && samples.back().t > until) samples.pop_back();
                return samples;
            };
            std::vector<std::vector<PlotPoint>> fitted_samples;
            if (auto_y) {
                // Y limits must be set before ImPlot lays out the axes. Fit and draw the
                // same pixel-bounded samples, even if the producer appends between those steps.
                const float decorations = std::max(0.0f, frame_size.x - plot_size.x);
                const auto fit_width = static_cast<std::size_t>(std::max(1.0f, plot_area.x - decorations));
                fitted_samples.resize(visible.size());
                AxisRange values{infinity, -infinity};
                for (std::size_t i = 0; i < visible.size(); ++i) {
                    if (!visible[i]) continue;
                    fitted_samples[i] = read_trace(i, fit_width);
                    extend_visible_y(values, fitted_samples[i], {left, until});
                }
                if (live.range.empty && !paused) { fitted_y = false; y = {-1, 1}; }
                fit_y(values, paused && (limits.x.min != last_limits.x.min || limits.x.max != last_limits.x.max));
                limits.y = y;
            }
            constexpr auto axis = ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoHighlight |
                ImPlotAxisFlags_NoMenus | ImPlotAxisFlags_NoSideSwitch |
                ImPlotAxisFlags_NoInitialFit | ImPlotAxisFlags_NoGridLines;
            ImPlot::SetupAxes(nullptr, nullptr, axis, axis);
            ImPlot::SetupAxesLimits(left, limits.x.max,
                limits.y.min, limits.y.max, ImPlotCond_Always);
            ImPlot::SetupLegend(ImPlotLocation_NorthWest, ImPlotLegendFlags_Horizontal |
                ImPlotLegendFlags_NoButtons | ImPlotLegendFlags_NoMenus |
                ImPlotLegendFlags_NoHighlightItem | ImPlotLegendFlags_NoHighlightAxis);
            ImPlot::SetupFinish();
            const auto pos = ImPlot::GetPlotPos();
            const auto size = ImPlot::GetPlotSize();
            plot_pos = pos;
            plot_size = size;
            const auto width = static_cast<std::size_t>(std::max(1.0f, size.x));
            const float dpi = ImGui::GetStyle().FontScaleDpi;
            const double seconds_per_pixel = std::max(std::numeric_limits<double>::min(),
                (limits.x.max - left) / std::max(1.0f, size.x));
            for (std::size_t i = 0; i < visible.size(); ++i) {
                if (!visible[i]) continue;
                // Preserve real samples beside the viewport for clipped lines.
                const auto samples = auto_y ? std::move(fitted_samples[i]) : read_trace(i, width);
                const auto color = signal_colors[i % signal_colors.size()];
                ImPlotSpec spec;
                spec.LineColor = color;
                spec.LineColor.w = line_alpha;
                spec.LineWeight = dpi;
                spec.Marker = ImPlotMarker_None;
                spec.Stride = sizeof(PlotPoint);
                const auto label = store.label(i) + "###channel" + std::to_string(i);
                // Keep the legend stable for registered channels without data.
                if (samples.empty()) {
                    ImPlot::PlotLine<double>(label.c_str(), nullptr, nullptr, 0, spec);
                    continue;
                }
                // Use the original sample period, including while inspecting frozen history.
                const double rate = paused ? frozen.sample_rate(i) : store.source(i).data->sample_rate();
                for (std::size_t begin = 0; begin < samples.size();) {
                    while (begin < samples.size() && !std::isfinite(samples[begin].value)) ++begin;
                    auto end = begin;
                    while (end < samples.size() && std::isfinite(samples[end].value)) ++end;
                    if (end > begin) {
                        const auto run = std::span(samples).subspan(begin, end - begin);
                        ImPlot::PlotLine(label.c_str(), &run.front().t, &run.front().value,
                            static_cast<int>(run.size()), spec);
                        draw_sample_markers(run, rate, seconds_per_pixel, dpi, color);
                        points += run.size();
                    }
                    begin = end;
                }
            }
            auto* draw = ImPlot::GetPlotDrawList();
            draw->AddLine(pos, {pos.x, pos.y + size.y}, IM_COL32(135, 135, 135, 255));
            draw->AddLine({pos.x, pos.y + size.y}, {pos.x + size.x, pos.y + size.y}, IM_COL32(135, 135, 135, 255));
            if (points == 0) {
                const char* hint = "暂无数据";
                const auto text = ImGui::CalcTextSize(hint);
                ImPlot::PushPlotClipRect();
                draw->AddText({pos.x + (size.x - text.x) * 0.5f, pos.y + (size.y - text.y) * 0.5f},
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), hint);
                ImPlot::PopPlotClipRect();
            }
            if (paused) {
                const char* hint = "已暂停";
                const auto text = ImGui::CalcTextSize(hint);
                draw->AddText({pos.x + size.x - text.x - 8, pos.y + 8},
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), hint);
            }
            last_limits = limits;
            last_drawn_until = until;
            displayed = true;
            const auto* plot = ImPlot::GetCurrentPlot();
            ImPlot::EndPlot();
            legend_pos = plot->Items.Legend.RectClamped.Min;
            const auto legend_end = plot->Items.Legend.RectClamped.Max;
            legend_size = {legend_end.x - legend_pos.x, legend_end.y - legend_pos.y};
            frame_pos = ImGui::GetItemRectMin();
            const auto frame_end = ImGui::GetItemRectMax();
            frame_size = {frame_end.x - frame_pos.x, frame_end.y - frame_pos.y};
        } else {
            drag = {};
            pan = {};
            frame_size = {};
        }
        ImGui::PopID();
    }
};

WaveView::WaveView(DataStore& store, ViewOptions options) : impl_(std::make_unique<Impl>(store, options)) {}
WaveView::WaveView(SignalHub& store, ViewOptions options) : impl_(std::make_unique<Impl>(store, options)) {}
WaveView::~WaveView() = default;
void WaveView::draw() { impl_->draw(); }
void WaveView::set_time_span(double seconds) {
    validate_span(seconds);
    impl_->automatic_span = false;
    impl_->options.time_span = seconds;
    if (impl_->paused) impl_->paused_x.min = impl_->paused_x.max - seconds;
    impl_->drag = {};
    impl_->pan = {};
}
void WaveView::set_y_range(double minimum, double maximum) {
    validate_y(minimum, maximum);
    impl_->auto_y = false;
    impl_->y = {minimum, maximum};
    impl_->drag = {};
    impl_->pan = {};
}
void WaveView::set_auto_y(bool enabled) {
    impl_->auto_y = enabled;
    impl_->fitted_y = false;
    impl_->shrink_since = -1;
    impl_->drag = {};
    impl_->pan = {};
}
void WaveView::set_zoom_enabled(bool enabled) {
    impl_->options.zoom_enabled = enabled;
    impl_->drag = {};
}
void WaveView::set_pan_enabled(bool enabled) {
    impl_->options.pan_enabled = enabled;
    impl_->pan = {};
}
void WaveView::set_pause_on_double_click(bool enabled) { impl_->options.pause_on_double_click = enabled; }
void WaveView::set_paused(bool paused) { impl_->set_paused(paused); }
bool WaveView::paused() const { return impl_->paused; }
double WaveView::time_span() const {
    return impl_->paused ? impl_->paused_x.max - impl_->paused_x.min : impl_->live_span();
}
void WaveView::set_channel_visible(std::size_t channel, bool visible) {
    impl_->sync_channels();
    if (channel >= impl_->visible.size()) throw std::out_of_range("Invalid channel index");
    if (impl_->visible[channel] != visible) impl_->fitted_y = false;
    impl_->visible[channel] = visible;
}
bool WaveView::channel_visible(std::size_t channel) const {
    impl_->sync_channels();
    if (channel >= impl_->visible.size()) throw std::out_of_range("Invalid channel index");
    return impl_->visible[channel];
}
std::size_t WaveView::points_this_frame() const { return impl_->points; }
} // namespace wave
