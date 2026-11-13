#ifdef _WIN32
#ifndef WINVER
#define WINVER 0x0603
#endif
#include "win32_touchpad.hpp"
#undef near // Win32's legacy memory-model macro conflicts with the numerical comparison below.
#endif
#include "wave_view.hpp"
#include "plot_navigation.hpp"
#include "signal_hub.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "implot.h"
#include "implot_internal.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
bool near(double a, double b) {
    return std::abs(a - b) <= 2e-6 * std::max({1.0, std::abs(a), std::abs(b)});
}
struct State { ImRect rect; ImPlotRange x, y; };
class Fixture {
public:
    wave::DataStore data{20'000, 5, {{"A", "V"}, {"B", "A"}}};
    std::unique_ptr<wave::WaveView> view, second;
    ImRect button;
    int host_clicks = 0;
    bool scrollable_host = false;
    bool standalone_canvas = false;
    float frame_delta = 1.0f / 60;

    explicit Fixture(wave::SignalHub* hub = nullptr, bool canvas = false,
                     wave::ViewOptions options = {.time_span = .2, .y_min = -10, .y_max = 10}) : standalone_canvas(canvas) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImPlot::CreateContext();
        auto& io = ImGui::GetIO();
        io.DisplaySize = {1280, 800};
        io.DeltaTime = 1.0f / 60;
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigInputTrickleEventQueue = false;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
        if (canvas && std::filesystem::exists("C:/Windows/Fonts/msyh.ttc"))
            io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/msyh.ttc", 16);
        else io.Fonts->AddFontDefault();
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        io.Fonts->SetTexID(static_cast<ImTextureID>(1));
        view = hub ? std::make_unique<wave::WaveView>(*hub, options)
                   : std::make_unique<wave::WaveView>(data, options);
        if (!hub) append(0, 20'000);
        for (int i = 0; i < 5; ++i) frame();
    }
    ~Fixture() {
        second.reset();
        view.reset();
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
    }
    void append(std::uint64_t first, std::size_t count, float amplitude = 1) {
        std::vector<float> values(count * 2);
        for (std::size_t i = 0; i < count; ++i) {
            values[i * 2] = amplitude * static_cast<float>(std::sin((first + i) * 0.02));
            values[i * 2 + 1] = -values[i * 2];
        }
        data.append(first, values);
    }
    void frame(wave::TouchpadDelta fingers) { render(fingers); }
    void frame() { render(std::nullopt); }
    void render(std::optional<wave::TouchpadDelta> fingers) {
        ImGui::GetIO().DeltaTime = frame_delta;
        ImGui::NewFrame();
        if (standalone_canvas) {
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
            ImGui::Begin("Canvas", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            if (fingers) view->draw(*fingers); else view->draw();
            ImGui::End();
            ImGui::PopStyleVar();
        } else {
        ImGui::SetNextWindowPos({110, 65});
        ImGui::SetNextWindowSize({1000, 650});
        const auto scroll_flags = scrollable_host ? ImGuiWindowFlags_HorizontalScrollbar : ImGuiWindowFlags_None;
        ImGui::Begin("Host", nullptr, ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | scroll_flags);
        if (ImGui::Button("Host control")) ++host_clicks;
        button = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
        ImGui::BeginChild("left", {second ? 470.0f : 0.0f, scrollable_host ? 300.0f : 0.0f});
        if (fingers) view->draw(*fingers); else view->draw();
        ImGui::EndChild();
        if (second) {
            ImGui::SameLine();
            ImGui::BeginChild("right", {0, 0});
            if (fingers) second->draw(*fingers); else second->draw();
            ImGui::EndChild();
        }
        if (scrollable_host) ImGui::Dummy({1600, 1000});
        ImGui::End();
        }
        ImGui::Render();
        require(GImGui->ErrorCountCurrentFrame == 0, "The component must not trigger ImGui recovery errors");
        const auto* draw = ImGui::GetDrawData();
        for (int list = 0; list < draw->CmdListsCount; ++list)
            for (const auto& vertex : draw->CmdLists[list]->VtxBuffer)
                require(std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y),
                    "Draw data must contain only finite vertex positions");
    }
    std::vector<State> states() const {
        std::vector<State> result;
        for (int i = 0; i < GImPlot->Plots.GetMapSize(); ++i) {
            if (const auto* p = GImPlot->Plots.TryGetMapData(i))
                result.push_back({p->PlotRect, p->Axes[ImAxis_X1].Range, p->Axes[ImAxis_Y1].Range});
        }
        std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
            return a.rect.Min.x < b.rect.Min.x;
        });
        return result;
    }
    State state() const { return states().at(0); }
    void move(ImVec2 p) { ImGui::GetIO().AddMousePosEvent(p.x, p.y); frame(); }
    void mouse(int button_id, bool down) {
        ImGui::GetIO().AddMouseButtonEvent(button_id, down);
        frame();
    }
    void click(int button_id, ImVec2 pos) { move(pos); mouse(button_id, true); mouse(button_id, false); }
    void double_click(ImVec2 point) {
        move(point);
        for (int i = 0; i < 24; ++i) frame();
        mouse(ImGuiMouseButton_Left, true);
        mouse(ImGuiMouseButton_Left, false);
        mouse(ImGuiMouseButton_Left, true);
        mouse(ImGuiMouseButton_Left, false);
    }
    void key(ImGuiKey id, bool down) { ImGui::GetIO().AddKeyEvent(id, down); frame(); }
};

double span(wave::AxisRange range) { return range.max - range.min; }
double fraction(wave::AxisRange range, double anchor) { return (anchor - range.min) / span(range); }
bool same(wave::PlotRange a, wave::PlotRange b) {
    return a.x.min == b.x.min && a.x.max == b.x.max &&
           a.y.min == b.y.min && a.y.max == b.y.max;
}
bool finite(wave::AxisRange range) {
    return std::isfinite(range.min) && std::isfinite(range.max) &&
           std::isfinite(span(range)) && range.max > range.min;
}

void direction_and_anchor() {
    const wave::PlotRange initial{{-2, 8}, {-10, 10}};
    const wave::ZoomAnchor anchor{1, 4};
    const auto zoomed = wave::zoom_from_drag(initial, anchor, 150, -90);
    require(span(zoomed.x) < span(initial.x) && span(zoomed.y) < span(initial.y),
            "Right/up dragging must zoom into X/Y");
    require(near(fraction(initial.x, anchor.x), fraction(zoomed.x, anchor.x)) &&
            near(fraction(initial.y, anchor.y), fraction(zoomed.y, anchor.y)),
            "The pressed data anchor must keep its viewport position");
    const auto expanded = wave::zoom_from_drag(initial, anchor, -150, 90);
    require(span(expanded.x) > span(initial.x) && span(expanded.y) > span(initial.y),
            "Left/down dragging must zoom out of X/Y");
    require(near(span(zoomed.x) * span(expanded.x), span(initial.x) * span(initial.x)),
            "Opposite drag distances must produce reciprocal exponential scales");
}

void return_to_origin_and_event_independence() {
    const wave::PlotRange initial{{10, 40}, {-5, 15}};
    const wave::ZoomAnchor anchor{13, 9};
    const auto direct = wave::zoom_from_drag(initial, anchor, 180, -90);
    wave::PlotRange staged = initial;
    for (int step = 1; step <= 60; ++step)
        staged = wave::zoom_from_drag(initial, anchor, 3.0 * step, -1.5 * step);
    require(same(direct, staged), "Final zoom must not depend on frame/event subdivision");
    const auto back_at_press = wave::zoom_from_drag(initial, anchor, 0, 0);
    require(same(back_at_press, initial), "Dragging back to the press must exactly restore initial bounds");
    const auto x_only = wave::zoom_from_drag(initial, anchor, 200, 0);
    const auto y_only = wave::zoom_from_drag(initial, anchor, 0, -200);
    require(x_only.y.min == initial.y.min && x_only.y.max == initial.y.max &&
            y_only.x.min == initial.x.min && y_only.x.max == initial.x.max,
            "A zero component must preserve the corresponding range exactly");
}

void extreme_deltas_and_limits() {
    const wave::PlotRange initial{{0, 10}, {-10, 10}};
    const wave::ZoomAnchor anchor{3, 2};
    wave::DragZoomLimits limits;
    limits.max_x_span = 120;
    limits.max_y_span = 500;
    const auto zoomed = wave::zoom_from_drag(initial, anchor, 1e308, -1e308, limits);
    const auto expanded = wave::zoom_from_drag(initial, anchor, -1e308, 1e308, limits);
    require(finite(zoomed.x) && finite(zoomed.y) && finite(expanded.x) && finite(expanded.y),
            "Extreme mouse deltas must always produce finite, ordered bounds");
    require(near(span(expanded.x), limits.max_x_span) &&
            near(span(expanded.y), limits.max_y_span),
            "X and Y must still respect their configured maximum spans");
    require(zoomed.x.min == std::nextafter(anchor.x, -std::numeric_limits<double>::infinity()) &&
            zoomed.x.max == std::nextafter(anchor.x, std::numeric_limits<double>::infinity()) &&
            zoomed.y.min == std::nextafter(anchor.y, -std::numeric_limits<double>::infinity()) &&
            zoomed.y.max == std::nextafter(anchor.y, std::numeric_limits<double>::infinity()),
            "Precision exhaustion must retain the anchor between adjacent representable coordinates");
    require(near(fraction(expanded.y, anchor.y), fraction(initial.y, anchor.y)),
            "Hitting the maximum span must still retain the anchor");
    require(same(wave::zoom_from_drag(initial, anchor, 0, 0, limits), initial),
            "Dragging back from a clamped limit must have no accumulated zoom state");

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const auto invalid = wave::zoom_from_drag({{nan, nan}, {5, 5}}, {nan, nan}, nan, nan);
    require(finite(invalid.x) && finite(invalid.y), "Invalid initial limits must recover to usable bounds");
}

void zoom_has_no_minimum_scale() {
    const wave::PlotRange initial{{54, 56}, {-.01, .03}};
    const wave::ZoomAnchor anchor{55.2, .007};
    const auto small = wave::zoom_from_drag(initial, anchor, 3000, -3000);
    const auto smaller = wave::zoom_from_drag(small, anchor, 100, -100);
    require(finite(small.x) && finite(small.y) && span(small.x) < 1.0 / 20'000 && span(small.y) < 1e-6,
            "X must zoom below one sample interval and Y below its former fixed minimum");
    require(span(smaller.x) < span(small.x) && span(smaller.y) < span(small.y),
            "Zoom must continue shrinking below both former minimum scales");
    const wave::PlotRange tiny{{0, 1e-200}, {0, 1e-200}};
    const auto tinier = wave::zoom_from_drag(tiny, {0, 0}, 180, -180);
    require(finite(tinier.x) && finite(tinier.y) && span(tinier.x) < span(tiny.x) && span(tinier.y) < span(tiny.y),
            "Valid tiny coordinate ranges must not be inflated to a fixed numerical floor");
}

void steady_plot_geometry() {
    for (bool canvas : {false, true}) {
        Fixture f(nullptr, canvas);
        const auto initial = f.state().rect;
        for (int frame = 0; frame < 120; ++frame) {
            f.frame();
            require(f.view->points_this_frame() > 0, "A visible static plot must submit data on every frame");
            const auto rect = f.state().rect;
            if (rect.Min.x != initial.Min.x || rect.Min.y != initial.Min.y ||
                rect.Max.x != initial.Max.x || rect.Max.y != initial.Max.y) {
                std::cerr << "frame=" << frame << " canvas=" << canvas << " initial="
                    << initial.Min.x << "," << initial.Min.y << "," << initial.Max.x << "," << initial.Max.y
                    << " now=" << rect.Min.x << "," << rect.Min.y << "," << rect.Max.x << "," << rect.Max.y << "\n";
                throw std::runtime_error("Static plot rectangle must not jitter between frames");
            }
        }
    }
    for (float dpi : {1.0f, 1.25f, 1.5f, 2.0f}) {
        wave::SignalHub empty;
        Fixture f(&empty, true);
        wave::apply_style(dpi);
        for (int frame = 0; frame < 10; ++frame) f.frame();
        const auto rect = f.state().rect;
        const auto vertices = ImGui::GetDrawData()->TotalVtxCount;
        for (int frame = 0; frame < 120; ++frame) {
            f.frame();
            const auto now = f.state().rect;
            require(now.Min.x == rect.Min.x && now.Min.y == rect.Min.y &&
                    now.Max.x == rect.Max.x && now.Max.y == rect.Max.y,
                "An empty canvas must retain its plot rectangle at every DPI");
            require(ImGui::GetDrawData()->TotalVtxCount == vertices && vertices > 0,
                "An empty canvas must render its axes and hint on every frame");
        }
    }
    std::cout << "PASS steady plot geometry in embedded and frameless canvases\n";
}


void fast_audio_narrow_view_has_stable_layout() {
    wave::SignalHub hub;
    Fixture f(&hub, true);
    f.view->set_time_span(.0068);
    f.view->set_y_range(-.116, .158);
    auto& audio = hub.register_source("audio", "Audio", 48'000,
        {{"Audio Left", "", "left"}, {"Audio Right", "", "right"}}, false);
    audio.online = true;
    audio.offset = 23.6;
    std::uint64_t next = 0;
    const auto append = [&] {
        std::vector<float> values(1920 * 2);
        for (std::size_t i = 0; i < 1920; ++i) {
            const double phase = (next + i) * 2 * 3.14159265358979323846 * 1000 / 48'000;
            values[i * 2] = static_cast<float>(.087 * std::sin(phase));
            values[i * 2 + 1] = static_cast<float>(.049 * std::sin(phase));
        }
        audio.data->append(next, values);
        next += 1920;
    };
    append();
    for (int i = 0; i < 10; ++i) f.frame();
    const auto initial = f.state().rect;
    int changes = 0;
    for (int frame = 0; frame < 600; ++frame) {
        if ((frame + 1) * 25 / 60 > frame * 25 / 60) append();
        f.frame();
        const auto now = f.state().rect;
        if (now.Min.x != initial.Min.x || now.Min.y != initial.Min.y ||
            now.Max.x != initial.Max.x || now.Max.y != initial.Max.y) {
            if (++changes < 6)
                std::cerr << "audio layout frame=" << frame << " initial=" << initial.Min.x << ","
                    << initial.Min.y << "," << initial.Max.x << "," << initial.Max.y << " now="
                    << now.Min.x << "," << now.Min.y << "," << now.Max.x << "," << now.Max.y << "\n";
        }
    }
    require(changes == 0, "Fast audio at the reported narrow scales must not move the plot rectangle");
    std::cout << "PASS stable layout with 48 kHz audio, 40 ms batches and a 6.8 ms view\n";
}



// The AA stroke uses the line atlas, while marker faces and fringes use its white texel.
constexpr int trace_alpha8 = 179; // 70% opacity in the rendered vertex colors.
constexpr int full_marker_alpha8 = 242; // 95% opacity in the rendered vertex colors.
int rendered_alpha(ImRect area, ImU32 color, bool markers) {
    int alpha = 0;
    for (const auto* list : ImGui::GetDrawData()->CmdLists) {
        const auto white = list->_Data->TexUvWhitePixel;
        for (const auto& vertex : list->VtxBuffer)
            if ((vertex.uv.x == white.x && vertex.uv.y == white.y) == markers &&
                ((vertex.col ^ color) & ~IM_COL32_A_MASK) == 0 && area.Contains(vertex.pos))
                alpha = std::max(alpha, static_cast<int>(vertex.col >> IM_COL32_A_SHIFT));
    }
    return alpha;
}

void sample_markers_follow_horizontal_zoom() {
    Fixture f;
    const auto& signal = *GImPlot->Plots.TryGetMapData(0)->Items.GetItemByIndex(0);
    const auto alpha = [&](bool markers) {
        auto area = f.state().rect;
        area.Min.y += area.GetHeight() / 4;
        return rendered_alpha(area, signal.Color, markers);
    };
    const auto set_spacing = [&](double dip) {
        // Changing DPI or tick labels can change the available plot width.
        for (int pass = 0; pass < 2; ++pass) {
            f.view->set_time_span(f.state().rect.GetWidth() /
                (dip * ImGui::GetStyle().FontScaleDpi * f.data.sample_rate()));
            f.frame();
        }
    };
    require(alpha(false) == trace_alpha8 && alpha(true) == 0, "A resolved line with dense samples must retain the base stroke without markers");
    f.view->set_paused(true);
    set_spacing(2.1);
    require(alpha(true) > 0 && alpha(true) < 25, "Markers must remain faintly visible at one diameter of spacing");
    set_spacing(9);
    require(alpha(false) == trace_alpha8 && alpha(true) == full_marker_alpha8, "Resolved markers must reach 95 percent opacity");
    const auto rect = f.state().rect;
    f.move({rect.GetCenter().x, rect.Max.y + 10});
    for (int direction : {-1, 1}) {
        int previous = alpha(true), intermediate_steps = 0;
        for (int tick = 0; tick < 10; ++tick) {
            ImGui::GetIO().AddMouseWheelEvent(static_cast<float>(-direction), 0);
            f.frame();
            const int current = alpha(true);
            const int change = (current - previous) * direction;
            require(change >= 0 && change < 65 && alpha(false) == trace_alpha8,
                "Successive wheel steps must fade dots monotonically and keep the stroke opacity constant");
            if (change > 0 && current > 0 && current < full_marker_alpha8) ++intermediate_steps;
            previous = current;
        }
        require(intermediate_steps >= 5 && previous == (direction < 0 ? 0 : full_marker_alpha8),
            "The real wheel gesture must pass through intermediate marker strengths before reaching its endpoint");
    }
    set_spacing(4.2);
    const auto mid = alpha(true);
    require(mid >= 137 && mid <= 145, "At two diameters markers should have about 55 percent opacity");
    wave::apply_style(2);
    f.frame();
    set_spacing(4.2);
    require(std::abs(alpha(true) - mid) <= 2,
        "Equal logical spacing must produce the same intermediate opacity at double DPI");
    set_spacing(14);
    f.view->set_y_range(-1e6, 1e6);
    f.frame();
    require(alpha(true) == full_marker_alpha8, "Changing the Y range must keep the horizontal density policy");
    f.view->set_paused(false);
    f.view->set_time_span(30);
    f.frame();
    require(alpha(true) == 0, "Returning to a live overview must hide resolved-sample markers");
    std::cout << "PASS continuous sample markers with a stable stroke across wheel zoom, Y zoom and DPI changes\n";
}

void irregular_sample_markers_respect_bursts() {
    wave::SignalHub hub;
    Fixture f(&hub);
    auto& source = hub.register_source("events", "Events", 0, {{"A", "V", "a"}}, true);
    source.online = true;
    const float value = 1;
    std::uint64_t next = 0;
    const double middle_gap = 4.2 * 1.05 / f.state().rect.GetWidth();
    for (double t : {.1, .5, .50001, .50002, .7, .7 + middle_gap, .9, 1.1})
        source.data->append_received(next++, std::span<const float>(&value, 1), t);
    f.view->set_time_span(1.05);
    f.frame();
    const auto& signal = *GImPlot->Plots.TryGetMapData(0)->Items.GetItemByIndex(0);
    const auto marker_near = [&](double t) {
        const auto state = f.state();
        const double x = state.rect.Min.x + (t - state.x.Min) / state.x.Size() * state.rect.GetWidth();
        auto strip = state.rect;
        strip.Min.x = static_cast<float>(x - 2);
        strip.Max.x = static_cast<float>(x + 2);
        strip.Min.y += strip.GetHeight() / 4;
        return rendered_alpha(strip, signal.Color, true);
    };
    require(marker_near(.9) == full_marker_alpha8 && marker_near(.7) > 120 && marker_near(.7) < 165 && marker_near(.5) == 0,
        "Irregular intervals must give full, partial and absent markers within one trace");
    const auto width = static_cast<std::uint64_t>(f.state().rect.GetWidth());
    for (; next < width + 1; ++next)
        source.data->append_received(next, std::span<const float>(&value, 1), 1.1 + next * 1e-6);
    f.frame();
    require(marker_near(.9) == full_marker_alpha8, "Crossing one sample per pixel must preserve isolated markers");
    for (; next < 6 * width; ++next)
        source.data->append_received(next, std::span<const float>(&value, 1), 1.1 + next * 1e-6);
    f.frame();
    require(marker_near(.9) == full_marker_alpha8 && marker_near(.7) > 120 && marker_near(.7) < 165 &&
            marker_near(.5) == 0 && marker_near(1.102) == 0,
        "Pixel reduction of a remote burst must preserve locally resolved markers and keep dense intervals plain");
    f.view->set_paused(true);
    f.frame();
    require(marker_near(.9) == full_marker_alpha8 && marker_near(.5) == 0, "Frozen receiver-timed data must use the same density");
    f.view->set_paused(false);
    hub.clear();
    source.data->append_received(0, std::span<const float>(&value, 1), 2);
    f.frame();
    require(marker_near(2) == full_marker_alpha8,
        "A single sample must stay visible before there is a second point to connect");
    std::cout << "PASS markers for isolated readings and the first sample of an irregular stream\n";
}

void overlap_compensation_is_local_to_the_trace() {
    wave::SignalHub hub;
    Fixture f(&hub, false, {.time_span = 1, .y_min = -2, .y_max = 2});
    auto& source = hub.register_source("input", "Input", 50'000,
        {{"Burst", "V", "burst"}, {"Ramp", "V", "ramp"}}, false);
    source.online = true;
    std::vector<float> data(50'000 * 2);
    for (std::size_t i = 0; i < 50'000; ++i) {
        const float ramp = static_cast<float>(i) / 50'000 - .5f;
        data[2 * i] = i < 25'000 ? ramp : (i % 2 ? 1.0f : -1.0f);
        data[2 * i + 1] = ramp;
    }
    source.data->append(0, data);
    for (bool paused : {false, true}) {
        f.view->set_paused(paused);
        f.frame();
        const auto rect = f.state().rect;
        auto& items = GImPlot->Plots.TryGetMapData(0)->Items;
        const auto ink = [&](int channel, float x) {
            const ImRect area{{rect.Min.x + x * rect.GetWidth(), rect.Min.y + 15},
                              {rect.Min.x + (x + .1f) * rect.GetWidth(), rect.Max.y - 15}};
            return rendered_alpha(area, items.GetItemByIndex(channel)->Color, false);
        };
        const int burst = ink(0, .7f);
        require(burst > 0 && burst < trace_alpha8 && ink(0, .2f) == trace_alpha8 && ink(1, .7f) == trace_alpha8,
            "Only retraced pixels may reduce line opacity, including inside a paused mixed-density view");
    }
    std::cout << "PASS local opacity compensation for a dense burst beside two resolved ramps\n";
}

void default_view_fills_then_scrolls() {
    wave::SignalHub hub;
    Fixture f(&hub, true, {});
    require(near(f.state().x.Min, 0) && near(f.state().x.Max, 10), "Empty default canvas must show ten seconds");
    auto& source = hub.register_source("v", "Voltage", 10, {{"V", "", "v"}}, false);
    source.online = true;
    source.offset = 25;
    source.data->append(0, std::vector<float>{3});
    f.frame();
    require(near(f.state().x.Min, 25) && near(f.state().x.Max, 35) &&
            f.state().y.Min < 3 && f.state().y.Max > 3, "The first point must start at the left and fit on Y");
    source.data->append(1, std::vector<float>(40, 3));
    f.frame();
    const auto during = f.state();
    const auto color = GImPlot->Plots.TryGetMapData(0)->Items.GetItemByIndex(0)->Color;
    auto tail = during.rect;
    tail.Min.y += 40;
    tail.Min.x += .4f * tail.GetWidth();
    tail.Max.x = tail.Min.x + 5;
    require(near(during.x.Min, 25) && near(during.x.Max, 35) && rendered_alpha(tail, color, true) > 0,
        "Four elapsed seconds must put the newest sample 40 percent across the stationary canvas");
    auto future = during.rect;
    future.Min.x += .45f * future.GetWidth();
    future.Min.y += 40;
    require(rendered_alpha(future, color, false) == 0 && rendered_alpha(future, color, true) == 0,
        "The unfilled portion of the canvas must be empty");
    f.view->set_paused(true);
    source.data->append(41, std::vector<float>(65, 30));
    f.frame();
    require(near(f.state().x.Min, 25) && near(f.state().y.Max, during.y.Max) &&
            rendered_alpha(future, color, false) == 0, "Pausing during the fill must freeze the trace and its axes");
    f.view->set_paused(false);
    f.frame();
    require(near(f.state().x.Min, 25.5) && near(f.state().x.Max, 35.5) && f.state().y.Max > 30,
        "Once ten seconds are filled the newest data must reach the right edge with its new amplitude in view");
    hub.clear();
    f.frame();
    source.data->append(500, std::vector<float>{-8});
    f.frame();
    require(near(f.state().x.Min, 75) && near(f.state().x.Max, 85) && f.state().y.Max < 0,
        "A new recording must start again at its own first timestamp and amplitude");
    std::cout << "PASS default ten-second left fill, empty future, pause, right-edge scroll and restart\n";
}

void default_span_uses_the_actual_frame_budget() {
    wave::DataStore standalone(48'000);
    wave::SignalHub hub;
    require(standalone.capacity_frames() == 50'000 && hub.capacity_frames() == 50'000,
        "Both default stores must allocate fifty thousand frames per channel");
    Fixture f(&hub, true, {});
    auto& source = hub.register_source("fast", "Fast", 50'000, {{"A", "", "a"}, {"B", "", "b"}}, false);
    source.online = true;
    source.data->append(0, std::vector<float>(40'000, .05f));
    f.frame();
    require(near(f.view->time_span(), 1) && near(f.state().x.Min, 0) && near(f.state().x.Max, 1),
        "The 50 kHz view must fill a one-second canvas from the left");
    source.data->append(20'000, std::vector<float>(100'000, .05f));
    f.frame();
    require(source.data->range().frame_count == 50'000 && near(f.state().x.Size(), 1) &&
            near(f.state().x.Max, source.data->range().end), "Both channels must share the full fifty-thousand-frame ring");
    hub.set_capacity_frames(25'000);
    f.frame();
    require(near(f.view->time_span(), .5) && near(f.state().x.Max, source.data->range().end),
        "Changing the frame budget must immediately update the automatic time scale");
    hub.set_capacity_frames(50'000);
    auto& slow = hub.register_source("slow", "Slow", 10'000, {{"S", "", "s"}}, false);
    slow.online = true;
    slow.data->append(0, std::vector<float>{1});
    f.frame();
    require(near(f.state().x.Size(), 1), "A mixed-rate plot must fit the shortest active visible history");
    f.view->set_channel_visible(0, false);
    f.view->set_channel_visible(1, false);
    f.frame();
    require(near(f.state().x.Size(), 5), "Hiding the 50 kHz source must expose five seconds for the 10 kHz source");
    f.view->set_time_span(.1);
    hub.set_capacity_frames(25'000);
    f.frame();
    require(near(f.state().x.Size(), .1), "A user-selected zoom must survive buffer resizing");
    std::cout << "PASS default 50k frame budget, rate-limited X scale and host capacity changes\n";
}

void irregular_full_ring_uses_real_elapsed_time() {
    wave::SignalHub hub;
    hub.set_capacity_frames(8);
    Fixture f(&hub, true, {});
    auto& source = hub.register_source("events", "Events", 0, {{"A", "", "a"}}, true);
    source.online = true;
    const std::vector<float> value{1};
    source.data->append_received(0, value, 3);
    f.frame();
    require(near(f.state().x.Min, 3) && near(f.state().x.Max, 13),
        "An irregular stream must start at its first reading without estimating a sampling rate");
    for (int i = 1; i < 8; ++i) source.data->append_received(i, value, 3 + .01 * i * i);
    f.frame();
    require(near(f.state().x.Min, 3) && near(f.state().x.Max, 3.49),
        "A full irregular ring must use the actual first and last timestamps");
    source.data->append_received(8, value, 3.71);
    f.frame();
    require(near(f.state().x.Min, 3.01) && near(f.state().x.Max, 3.71),
        "Irregular rollover must keep the latest value exactly on the right");
}

void auto_y_tracks_visible_amplitudes() {
    wave::SignalHub hub;
    Fixture f(&hub, true, {});
    auto& source = hub.register_source("both", "Signals", 10, {{"A", "", "a"}, {"B", "", "b"}}, false);
    source.online = true;
    source.data->append(0, std::vector<float>{-2, 200, 4, 210, 1, 220});
    f.frame();
    const auto combined = f.state().y;
    require(near(222 / combined.Size(), .8) && near((-2 - combined.Min) / combined.Size(), .1) &&
            near((combined.Max - 220) / combined.Size(), .1),
        "All visible channels together must fill 80 percent with ten percent above and below");
    f.view->set_channel_visible(1, false);
    f.frame();
    require(near(f.state().y.Min, -2.75) && near(f.state().y.Max, 4.75),
        "The remaining visible signal must occupy 80 percent after hiding a channel");
    auto plot = f.state();
    f.move({plot.rect.GetCenter().x, plot.rect.Max.y + 4});
    ImGui::GetIO().AddMouseWheelEvent(-1, 0);
    f.frame();
    source.data->append(3, std::vector<float>{50, 200});
    f.frame();
    require(f.state().y.Max > 50, "Horizontal movement must preserve auto Y and an incoming peak must fit on its first frame");
    const auto expanded = f.state().y;
    source.data->append(4, std::vector<float>(210, 1.0f));
    f.frame();
    require(near(f.state().y.Max, expanded.Max), "A departing transient must not immediately move the axes");
    for (int i = 0; i < 38; ++i) f.frame();
    require(f.state().y.Min < 1 && f.state().y.Max > 1 && f.state().y.Max < 2,
        "After the peak leaves the viewport auto Y must contract around the visible constant signal");
    plot = f.state();
    f.move({plot.rect.Min.x - 6, plot.rect.GetCenter().y});
    ImGui::GetIO().AddMouseWheelEvent(0, 1);
    f.frame();
    const auto manual = f.state().y;
    source.data->append(109, std::vector<float>{-300, 0});
    f.frame();
    require(near(f.state().y.Min, manual.Min) && near(f.state().y.Max, manual.Max), "Manual Y zoom must hold its scale");
    f.view->set_auto_y(true);
    f.frame();
    require(f.state().y.Min < -300, "The host must be able to restore automatic Y fitting");
    std::cout << "PASS auto Y across channels, first-frame peaks, quiet contraction and manual Y override\n";
}

void basic_display_and_host_api() {
    Fixture f;
    require(f.view->points_this_frame() > 0, "Registered signals must display immediately");
    auto s = f.state();
    require(near(s.x.Size(), .2) && near(s.x.Max, f.data.range().end), "X must follow the newest real data");
    require(near(s.y.Min, -10) && near(s.y.Max, 10), "Y must use fixed host limits");
    f.append(20'000, 20'000, 100);
    f.frame();
    require(near(f.state().y.Min, -10) && near(f.state().y.Max, 10), "Large input must not enable Y auto-fit");
    require(f.state().x.Max > s.x.Max, "New samples must move the live time axis");

    const auto all = f.view->points_this_frame();
    f.view->set_channel_visible(1, false);
    f.frame();
    require(f.view->points_this_frame() > 0 && f.view->points_this_frame() < all,
        "Host channel filtering must change plotted data");
    f.view->set_channel_visible(0, false);
    f.frame();
    require(f.view->points_this_frame() == 0, "Host may hide every channel");
    f.view->set_channel_visible(0, true);
    f.view->set_time_span(1.5);
    f.view->set_y_range(-200, 200);
    f.frame();
    require(near(f.state().x.Size(), 1.5) && near(f.state().y.Min, -200) && near(f.state().y.Max, 200),
        "Host range changes must take effect on the next draw");

    f.data.clear_channel(0);
    f.frame();
    require(f.view->points_this_frame() == 0, "Transport channel clear must not resurrect old samples");
    f.append(40'000, 20);
    f.frame();
    require(f.view->points_this_frame() > 0, "Reception must continue after transport clear");
    std::cout << "PASS live rendering, fixed Y, host ranges/visibility and continued reception\n";
}

void zoom_can_be_disabled() {
    Fixture f;
    f.view->set_zoom_enabled(false);
    f.view->set_pan_enabled(false);
    f.view->set_pause_on_double_click(false);
    f.frame();
    const auto before = f.state();
    const auto centre = before.rect.GetCenter();
    const auto assert_fixed = [&] {
        const auto now = f.state();
        require(near(now.x.Min, before.x.Min) && near(now.x.Max, before.x.Max) &&
                near(now.y.Min, before.y.Min) && near(now.y.Max, before.y.Max),
            "Plot mouse/keyboard input must not change the fixed viewport");
        require(GImGui->OpenPopupStack.empty(), "Plot input must not open menus");
        require(f.states().size() == 1, "Input must not create another plot or overview");
    };
    for (int button : {ImGuiMouseButton_Left, ImGuiMouseButton_Right, ImGuiMouseButton_Middle}) {
        f.move(centre);
        f.mouse(button, true);
        f.move({centre.x + 70, centre.y - 50});
        f.mouse(button, false);
        assert_fixed();
        f.click(button, centre);
        f.click(button, centre);
        assert_fixed();
    }
    for (auto pos : {centre, ImVec2{before.rect.Min.x - 12, centre.y},
                     ImVec2{centre.x, before.rect.Max.y + 10}}) {
        f.move(pos);
        ImGui::GetIO().AddMouseWheelEvent(0, 3);
        f.frame();
        assert_fixed();
    }
    for (auto key : {ImGuiKey_Space, ImGuiKey_Escape, ImGuiKey_LeftCtrl, ImGuiKey_LeftShift}) {
        f.key(key, true);
        f.key(key, false);
        assert_fixed();
    }
    const auto* plot = GImPlot->Plots.TryGetMapData(0);
    const auto legend = plot->Items.Legend.RectClamped.GetCenter();
    f.click(ImGuiMouseButton_Left, legend);
    f.click(ImGuiMouseButton_Right, legend);
    assert_fixed();
    require(f.view->channel_visible(0) && f.view->channel_visible(1), "Legend input must not hide signals");
    f.append(20'000, 20'000);
    f.frame();
    require(f.state().x.Max > before.x.Max && f.view->points_this_frame() > 0,
        "Double clicks and Space must never freeze subsequent reception");
    std::cout << "PASS host-disabled zoom, removed gestures and menus\n";
}


void wheel_zoom_across_panel() {
    for (int region = 0; region < 3; ++region) {
        Fixture f;
        const auto before = f.state();
        const ImVec2 point = region == 1
            ? ImVec2{before.rect.GetCenter().x, before.rect.Max.y + 10}
            : region == 2 ? ImVec2{before.rect.Min.x - 12, before.rect.Min.y + before.rect.GetHeight() * .35f}
                        : ImVec2{before.rect.GetCenter().x, before.rect.Min.y + before.rect.GetHeight() * .35f};
        f.move(point);
        const double y_fraction = std::clamp(double(ImGui::GetIO().MousePos.y - before.rect.Min.y) /
            before.rect.GetHeight(), 0.0, 1.0);
        const double anchor = before.y.Max - y_fraction * before.y.Size();
        ImGui::GetIO().AddMouseWheelEvent(0, 1);
        f.frame();
        auto after = f.state();
        const double scale = std::exp(-35.0 / 180);
        require(near(after.x.Size(), before.x.Size()) && near(after.y.Size(), before.y.Size() * scale),
            "Wheel up must magnify Y by the same amount anywhere on the plot or axes");
        require(near(after.x.Max, before.x.Max), "Wheel zoom must keep the newest data on the right edge");
        require(near((anchor - before.y.Min) / before.y.Size(), (anchor - after.y.Min) / after.y.Size()),
            "Y wheel zoom must preserve the cursor anchor, including at the edge");
        f.append(20'000, 20'000);
        f.frame();
        require(f.state().x.Max > before.x.Max && near(f.state().x.Size(), after.x.Size()) &&
                near(f.state().y.Min, after.y.Min) && near(f.state().y.Max, after.y.Max),
            "Wheel ranges must persist while live data continues");
        f.move(f.button.GetCenter());
        ImGui::GetIO().AddMouseWheelEvent(0, 2);
        f.frame();
        require(near(f.state().x.Size(), after.x.Size()) && near(f.state().y.Size(), after.y.Size()),
            "Wheel outside the assigned plot must not zoom it");
    }
    std::cout << "PASS identical wheel response throughout the panel, anchors and live following\n";
}

void two_finger_zoom() {
    Fixture f;
    const auto wheel = [&](float x, float y) {
        ImGui::GetIO().AddMouseWheelEvent(x, y);
        f.frame();
    };
    auto before = f.state();
    f.move(before.rect.GetCenter());
    wheel(-.25f, .125f);
    auto after = f.state();
    require(near(after.x.Size(), before.x.Size() * std::exp(-.25 * 35 / 180)) &&
            near(after.y.Size(), before.y.Size() * std::exp(-.125 * 35 / 180)) && near(after.x.Max, before.x.Max),
        "Upper-right wheel fallback must magnify both axes by their own component while following live data");
    wheel(.25f, 0);
    wheel(0, -.125f);
    require(near(f.state().x.Size(), before.x.Size()) && near(f.state().y.Min, before.y.Min) && near(f.state().y.Max, before.y.Max),
        "Lower-left fractions on separate frames must undo the diagonal");
    f.frame({12, 8});
    after = f.state();
    require(after.x.Size() < before.x.Size() && after.y.Size() < before.y.Size(),
        "The host's physical upper-right two-finger gesture must magnify both axes");
    f.append(20'000, 20'000);
    f.frame();
    require(f.state().x.Max > before.x.Max && near(f.state().x.Size(), after.x.Size()),
        "New data must keep scrolling at the selected time scale");
    f.view->set_paused(true);
    f.frame();
    before = f.state();
    const auto anchor_pos = ImVec2{before.rect.Min.x + before.rect.GetWidth() * .3f,
                                   before.rect.Min.y + before.rect.GetHeight() * .7f};
    f.move(anchor_pos);
    const double fx = (ImGui::GetIO().MousePos.x - before.rect.Min.x) / before.rect.GetWidth();
    const double fy = (before.rect.Max.y - ImGui::GetIO().MousePos.y) / before.rect.GetHeight();
    f.frame({-18, -9});
    after = f.state();
    require(after.x.Size() > before.x.Size() && after.y.Size() > before.y.Size() && f.view->paused() &&
            near(before.x.Min + fx * before.x.Size(), after.x.Min + fx * after.x.Size()) &&
            near(before.y.Min + fy * before.y.Size(), after.y.Min + fy * after.y.Size()),
        "Lower-left must show more history and amplitude around the paused cursor");
    f.frame({18, 9});
    require(near(f.state().x.Min, before.x.Min) && near(f.state().y.Min, before.y.Min) &&
            near(f.state().x.Max, before.x.Max) && near(f.state().y.Max, before.y.Max),
        "Reversing the physical slide must recover both axes");
    before = f.state();
    ImGui::GetIO().AddMouseWheelEvent(0, .25f);
    f.frame({});
    require(near(f.state().x.Size(), before.x.Size()) && f.state().y.Size() < before.y.Size(),
        "A fractional vertical wheel event must keep its direction with the explicit touchpad overload");
    before = f.state();
    f.move({before.rect.Min.x + before.rect.GetWidth() * .28f, before.rect.Max.y + 5});
    const double fraction = (ImGui::GetIO().MousePos.x - before.rect.Min.x) / before.rect.GetWidth();
    const double cursor_time = before.x.Min + fraction * before.x.Size();
    f.frame({18, -9});
    after = f.state();
    require(after.x.Size() < before.x.Size() && near(after.x.Min + fraction * after.x.Size(), cursor_time) &&
            after.y.Size() > before.y.Size() && f.view->paused(),
        "The bottom time axis must accept both components, including opposite directions");
    f.frame({-18, 9});
    require(near(f.state().x.Min, before.x.Min) && near(f.state().x.Max, before.x.Max),
        "Opposite fractional scroll must recover the paused range");
    f.move({before.rect.Min.x - 4, before.rect.GetCenter().y});
    f.frame({12, 8});
    const auto on_left = f.state();
    require(on_left.x.Size() < before.x.Size() && on_left.y.Size() < before.y.Size(),
        "The left amplitude scale must accept both components");
    f.move(GImPlot->Plots.TryGetMapData(0)->Items.Legend.RectClamped.GetCenter());
    f.frame({12, 8});
    require(near(f.state().x.Size(), on_left.x.Size()) && near(f.state().y.Size(), on_left.y.Size()),
        "Zoom must ignore the legend");
    f.view->set_zoom_enabled(false);
    f.move(before.rect.GetCenter());
    f.frame({12, 8});
    require(near(f.state().x.Size(), on_left.x.Size()) && near(f.state().y.Size(), on_left.y.Size()),
        "A disabled view must ignore both components");
    f.view->set_zoom_enabled(true);
    f.view->set_paused(false);
    f.view->set_auto_y(true);
    f.frame();
    f.move(f.state().rect.GetCenter());
    f.frame({12, 0});
    f.append(40'000, 20'000, 30);
    f.frame();
    require(f.state().y.Max > 29, "Two-finger horizontal zoom must leave automatic Y fitting active");
    f.view->set_y_range(-40, 40);
    f.frame();
    wheel(-.25f, 0);
    const auto before_notch = f.state();
    wheel(0, 1);
    require(f.state().y.Size() < before_notch.y.Size() && near(f.state().x.Size(), before_notch.x.Size()),
        "A whole vertical notch must follow the same component rule as fractional input");
    const auto before_vertical = f.state();
    wheel(0, .25f);
    require(f.state().y.Size() < before_vertical.y.Size() && near(f.state().x.Size(), before_vertical.x.Size()),
        "A directional vertical fraction must zoom Y alone");
    f.frame({-8, 8});
    require(f.state().x.Size() > before_vertical.x.Size() && f.state().y.Size() < before_vertical.y.Size(),
        "Upper-left must widen X while narrowing Y");
    std::cout << "PASS two-finger directional XY zoom, wheel fallback, hit regions and pause/live anchors\n";
}

#ifdef _WIN32
void win32_touchpad_contacts_drive_the_view() {
    Fixture f;
    f.view->set_paused(true);
    f.frame();
    const auto before = f.state();
    f.move(before.rect.GetCenter());
    wave::win32::TouchpadInput input;
    POINTER_TOUCH_INFO contacts[3]{};
    for (UINT32 i = 0; i < 3; ++i) {
        auto& p = contacts[i].pointerInfo;
        p.pointerId = i + 1;
        p.pointerType = PT_TOUCHPAD;
        p.pointerFlags = POINTER_FLAG_INCONTACT;
        p.ptHimetricLocation = {2000 + static_cast<LONG>(i) * 1000, 3000};
    }
    input.update(contacts, 2);
    for (int i = 0; i < 2; ++i) {
        contacts[i].pointerInfo.ptHimetricLocation.x += 254;
        contacts[i].pointerInfo.ptHimetricLocation.y -= 127;
    }
    std::swap(contacts[0], contacts[1]);
    input.update(contacts, 2);
    input.update(contacts, 2); // The same frame can be reported for both IDs.
    auto delta = input.take_delta();
    require(near(delta.right, 9.6) && near(delta.up, 4.8), "Contact motion must give right/up logical pixels exactly once");
    f.frame(delta);
    const auto after = f.state();
    require(after.x.Size() < before.x.Size() && after.y.Size() < before.y.Size(), "Physical upper-right must show detail");
    for (int i = 0; i < 2; ++i) {
        contacts[i].pointerInfo.ptHimetricLocation.x -= 254;
        contacts[i].pointerInfo.ptHimetricLocation.y += 127;
    }
    input.update(contacts, 2);
    f.frame(input.take_delta());
    require(near(f.state().x.Size(), before.x.Size()) && near(f.state().y.Size(), before.y.Size()),
        "Physical lower-left of the same length must restore the overview");
    contacts[0].pointerInfo.ptHimetricLocation.x += 150;
    contacts[1].pointerInfo.ptHimetricLocation.x -= 150;
    input.update(contacts, 2);
    input.update(contacts, 3);
    contacts[0].pointerInfo.ptHimetricLocation.x += 2000;
    input.update(contacts, 2);
    contacts[1].pointerInfo.pointerId = 4;
    contacts[1].pointerInfo.ptHimetricLocation.y += 800;
    input.update(contacts, 2);
    delta = input.take_delta();
    require(delta.right == 0 && delta.up == 0, "A centered pinch, third finger or replacement contact must not produce a pan");
    contacts[0].pointerInfo.ptHimetricLocation.y += 100;
    input.update(contacts, 2);
    input.handle(WM_KILLFOCUS, 0);
    input.update(contacts, 2);
    delta = input.take_delta();
    require(delta.right == 0 && delta.up == 0, "Focus loss must discard pending motion and rebaseline the contact pair");
    std::cout << "PASS Win32 physical touchpad contacts, reversibility, deduplication and cancellation\n";
}
#endif

void right_drag_matches_two_fingers_across_panel() {
    for (int region = 0; region < 3; ++region) {
        Fixture f;
        const auto before = f.state();
        const ImVec2 point = region == 1
            ? ImVec2{before.rect.GetCenter().x, before.rect.Max.y + 10}
            : region == 2 ? ImVec2{before.rect.Min.x - 12, before.rect.Min.y + before.rect.GetHeight() * .35f}
                        : ImVec2{before.rect.GetCenter().x, before.rect.Min.y + before.rect.GetHeight() * .35f};
        f.move(point);
        // Deliberately unequal components. Splitting a finger trajectory across
        // frames must have the same effect as one mouse drag, whichever region owns it.
        f.frame({30, 5});
        f.frame({30, 10});
        const auto fingers = f.state();
        require(near(fingers.x.Size() / before.x.Size(), std::exp(-60 * 1.95 / 180)) &&
                near(fingers.y.Size() / before.y.Size(), std::exp(-15 * 1.95 / 180)),
            "The amount on each axis must come from that component of displacement");
        f.frame({-60, -15});
        f.mouse(ImGuiMouseButton_Right, true);
        f.move({point.x + 60, point.y - 15});
        const auto after = f.state();
        require(near(after.x.Min, fingers.x.Min) && near(after.x.Max, fingers.x.Max) &&
                near(after.y.Min, fingers.y.Min) && near(after.y.Max, fingers.y.Max),
            "Identical mouse and finger displacement must yield identical bounds over the plot and both scales");
        require(near(after.x.Max, before.x.Max), "A held right drag must keep following the latest data");
        f.append(20'000, 20'000);
        f.frame();
        require(f.state().x.Max > after.x.Max && near(f.state().x.Size(), after.x.Size()),
            "Data arriving during a held right drag must advance without changing its span");
        f.move(point);
        require(near(f.state().x.Size(), before.x.Size()) &&
                near(f.state().y.Min, before.y.Min) && near(f.state().y.Max, before.y.Max),
            "Returning a drag to its start must restore the original scales");
        f.move({point.x - 50, point.y + 35});
        require(f.state().x.Size() > before.x.Size() && f.state().y.Size() > before.y.Size(),
            "Left/down dragging must widen both ranges anywhere on the panel");
        f.move({point.x + 60, point.y});
        require(f.state().x.Size() < before.x.Size() && near(f.state().y.Size(), before.y.Size()),
            "A horizontal right drag must only magnify X");
        f.move({point.x, point.y - 45});
        require(near(f.state().x.Size(), before.x.Size()) && f.state().y.Size() < before.y.Size(),
            "A vertical right drag must only magnify Y");
        f.mouse(ImGuiMouseButton_Right, false);
        require(GImGui->OpenPopupStack.empty(), "Right release must not open menus");
    }
    std::cout << "PASS per-axis displacement and identical right drag/touchpad scaling throughout the panel\n";
}

void unsupported_inputs_and_host_override() {
    Fixture f;
    const auto before = f.state();
    auto point = before.rect.GetCenter();
    for (int button : {ImGuiMouseButton_Middle}) {
        f.move(point);
        f.mouse(button, true);
        f.move({point.x + 80, point.y - 50});
        f.mouse(button, false);
        f.click(button, point);
        f.click(button, point);
    }
    f.key(ImGuiKey_Space, true);
    f.key(ImGuiKey_Space, false);
    require(near(f.state().x.Size(), before.x.Size()) && near(f.state().y.Size(), before.y.Size()) &&
            f.states().size() == 1 && GImGui->OpenPopupStack.empty(),
        "Pan, freeze, Auto, box selection and extra panels must remain removed");
    f.move(point);
    f.mouse(ImGuiMouseButton_Right, true);
    f.move({point.x + 50, point.y - 30});
    f.view->set_time_span(.5);
    f.view->set_y_range(-30, 30);
    f.move({point.x + 90, point.y - 60});
    require(near(f.state().x.Size(), .5) && near(f.state().y.Min, -30) && near(f.state().y.Max, 30),
        "Host setters must cancel a held gesture and take priority");
    f.mouse(ImGuiMouseButton_Right, false);
    f.append(20'000, 20'000);
    f.frame();
    require(f.state().x.Max > before.x.Max, "Unsupported double clicks and Space must not pause live data");
    f.second = std::make_unique<wave::WaveView>(f.data);
    for (int i = 0; i < 3; ++i) f.frame();
    const auto states = f.states();
    f.move(states[0].rect.GetCenter());
    ImGui::GetIO().AddMouseWheelEvent(0, 1);
    f.frame();
    require(near(f.states()[0].x.Size(), states[0].x.Size()) && f.states()[0].y.Size() < states[0].y.Size() &&
            near(f.states()[1].x.Size(), states[1].x.Size()) &&
            near(f.states()[1].y.Size(), states[1].y.Size()),
        "Zoom must affect only the hovered component instance");
    std::cout << "PASS unsupported gestures remain removed, host override and per-instance zoom\n";
}

void wheel_does_not_scroll_the_host() {
    Fixture f;
    f.scrollable_host = true;
    for (int i = 0; i < 3; ++i) f.frame();
    f.move(f.state().rect.GetCenter());
    ImGui::GetIO().AddMouseWheelEvent(0, -1);
    f.frame();
    require(ImGui::FindWindowByName("Host")->Scroll.y == 0,
        "Zooming the plot must not also scroll its containing host window");
    ImGui::GetIO().AddMouseWheelEvent(-1, 0);
    f.frame();
    auto* host = ImGui::FindWindowByName("Host");
    require(host->ScrollMax.x > 0 && host->Scroll.x == 0,
        "Horizontal time zoom must not also scroll a horizontally scrollable host");
    f.move({800, 640});
    f.frame();
    ImGui::GetIO().AddMouseWheelEvent(0, -1);
    f.frame();
    require(ImGui::FindWindowByName("Host")->Scroll.y > 0,
        "Wheel outside the plot must remain available to the host scroll region");
    const double time_span = f.state().x.Size();
    ImGui::GetIO().AddMouseWheelEvent(-1, 0);
    f.frame();
    require(host->Scroll.x > 0 && near(f.state().x.Size(), time_span),
        "Outside the plot a horizontal gesture must scroll the host and preserve the time span");
    std::cout << "PASS wheel ownership inside a scrolling host\n";
}


void pause_keeps_snapshot_and_resumes_latest() {
    Fixture f;
    const auto point = f.state().rect.GetCenter();
    f.double_click(point);
    require(f.view->paused(), "Double click must pause only this view");
    const auto paused = f.state();
    const auto points = f.view->points_this_frame();
    f.append(20'000, 200'000, 5);
    f.frame();
    require(f.data.range().first_frame > 20'000, "The live ring must continue overwriting while paused");
    require(near(f.state().x.Min, paused.x.Min) && near(f.state().x.Max, paused.x.Max) &&
            f.view->points_this_frame() == points && points > 0,
        "Paused samples must survive complete live-ring overwrite");
    f.view->set_time_span(.4);
    f.view->set_y_range(-8, 12);
    f.frame();
    f.double_click(f.state().rect.GetCenter());
    require(!f.view->paused() && near(f.state().x.Max, f.data.range().end) &&
            near(f.state().x.Size(), .4) && near(f.state().y.Min, -8) && near(f.state().y.Max, 12),
        "Resume must jump to the newest cache and preserve the selected scales");
    std::cout << "PASS display-only pause, live overwrite, stable snapshot and latest-cache resume\n";
}

void pan_live_and_paused() {
    Fixture f;
    auto before = f.state();
    auto point = before.rect.GetCenter();
    f.move(point);
    f.mouse(ImGuiMouseButton_Left, true);
    f.move({point.x + 80, point.y + 50});
    auto shifted = f.state();
    require(near(shifted.x.Min, before.x.Min) && near(shifted.x.Max, before.x.Max) &&
            shifted.y.Min > before.y.Min && near(shifted.y.Size(), before.y.Size()),
        "Live left drag must pan Y while X keeps following");
    f.append(20'000, 20'000);
    f.frame();
    require(f.state().x.Max > before.x.Max && near(f.state().x.Size(), before.x.Size()),
        "Receiving must continue during a held live pan");
    f.mouse(ImGuiMouseButton_Left, false);
    f.view->set_paused(true);
    f.frame();
    before = f.state();
    point = before.rect.GetCenter();
    for (int i = 0; i < 24; ++i) f.frame();
    f.move(point);
    f.mouse(ImGuiMouseButton_Left, true);
    f.move({point.x + 60, point.y - 40});
    shifted = f.state();
    require(shifted.x.Min < before.x.Min && shifted.y.Min < before.y.Min &&
            near(shifted.x.Size(), before.x.Size()) && near(shifted.y.Size(), before.y.Size()),
        "Paused left drag must pan both axes without changing their scales");
    f.mouse(ImGuiMouseButton_Left, false);
    const auto anchor_x = f.state().x.Min + .4 * f.state().x.Size();
    point = {f.state().rect.Min.x + .4f * f.state().rect.GetWidth(), f.state().rect.GetCenter().y};
    f.move(point);
    const auto initial = f.state();
    const double actual_anchor = initial.x.Min + (ImGui::GetIO().MousePos.x - initial.rect.Min.x) /
        initial.rect.GetWidth() * initial.x.Size();
    ImGui::GetIO().AddMouseWheelEvent(1, 0);
    f.frame();
    require(f.view->paused() && f.state().x.Size() > initial.x.Size() &&
            near((actual_anchor - initial.x.Min) / initial.x.Size(),
                 (actual_anchor - f.state().x.Min) / f.state().x.Size()) && std::isfinite(anchor_x),
        "Paused zoom must keep the mouse's X anchor without resuming");
    std::cout << "PASS live Y pan, paused XY pan and paused mouse-anchored zoom\n";
}

void resume_never_rewinds_to_a_delayed_clock() {
    Fixture f;
    f.view->set_paused(true);
    f.frame();
    std::uint64_t next = 20'000;
    for (int i = 0; i < 100; ++i) {
        f.append(next, 400);
        next += 400;
        f.frame();
    }
    f.move(f.state().rect.GetCenter());
    f.mouse(ImGuiMouseButton_Left, true);
    f.mouse(ImGuiMouseButton_Left, false);
    f.append(next, 400);
    f.mouse(ImGuiMouseButton_Left, true);
    require(!f.view->paused(), "The second click must resume the active stream");
    const auto resumed = f.state().x.Max;
    require(near(resumed, f.data.range().end), "Resume must use raw latest time rather than a buffered playhead");
    f.mouse(ImGuiMouseButton_Left, false);
    for (int i = 0; i < 20; ++i) {
        f.frame();
        require(f.state().x.Max >= resumed, "A resumed view must never jump backwards to the old display clock");
    }
    std::cout << "PASS immediate latest-cache resume without display-clock rewind\n";
}

void pause_input_scope_and_independent_views() {
    Fixture f;
    const auto* plot = GImPlot->Plots.TryGetMapData(0);
    f.double_click(plot->Items.Legend.RectClamped.GetCenter());
    require(!f.view->paused(), "Legend double click must not pause");
    f.second = std::make_unique<wave::WaveView>(f.data);
    for (int i = 0; i < 3; ++i) f.frame();
    f.view->set_paused(true);
    f.frame();
    const auto before = f.states();
    f.append(20'000, 20'000, 5.0f);
    f.frame();
    const auto after = f.states();
    require(f.view->paused() && !f.second->paused() &&
            near(after[0].x.Max, before[0].x.Max) && near(after[1].x.Max, before[1].x.Max) &&
            after[1].y.Max > 2 * before[1].y.Max,
        "Pausing one component must not freeze another sharing the same source");
    f.key(ImGuiKey_Space, true);
    f.key(ImGuiKey_Space, false);
    require(f.view->paused(), "Space remains disabled");
    std::cout << "PASS legend/input scope, independent paused/live views and disabled Space\n";
}

void embedding_is_local_and_independent() {
    Fixture f;
    const auto config = ImGui::GetIO().ConfigFlags;
    const auto menu = ImPlot::GetInputMap().Menu;
    const auto pan = ImPlot::GetInputMap().Pan;
    f.second = std::make_unique<wave::WaveView>(f.data,
        wave::ViewOptions{.time_span = 1, .y_min = -5, .y_max = 5});
    f.second->set_channel_visible(0, false);
    for (int i = 0; i < 4; ++i) f.frame();
    auto states = f.states();
    require(states.size() == 2, "Two host-owned child regions must contain two independent plots");
    require(states[0].rect.Min.x > 110 && states[1].rect.Max.x < 1110 &&
            states[0].rect.Min.y > 100 && states[1].rect.Max.y < 715,
        "The component must stay inside the host's assigned child area");
    require(near(states[0].x.Size(), .2) && near(states[1].x.Size(), 1) &&
            near(states[0].y.Max, 10) && near(states[1].y.Max, 5),
        "Instances must preserve independent ranges without linking");
    require(f.view->channel_visible(0) && !f.second->channel_visible(0),
        "One instance's selection must not affect another");
    f.second->set_time_span(2);
    f.frame();
    require(near(f.states()[0].x.Size(), .2), "Host range edits must not link independent instances");
    require(ImGui::GetIO().ConfigFlags == config && ImPlot::GetInputMap().Menu == menu &&
            ImPlot::GetInputMap().Pan == pan, "Drawing must preserve the host's global input setup");
    for (const auto* window : GImGui->Windows) {
        if (!window->Active) continue;
        require(window->Name[0] == '#' || std::string(window->Name).find("Host") == 0,
            "WaveView must not create top-level windows or menus");
    }
    f.click(ImGuiMouseButton_Left, f.button.GetCenter());
    require(f.host_clicks == 1, "Host controls must still receive normal input");
    std::cout << "PASS embedded layout, independent instances, unchanged host input and working host controls\n";
}

void mixed_sources_and_empty_states() {
    wave::SignalHub hub;
    Fixture f(&hub);
    require(f.view->points_this_frame() == 0, "An empty source registry must draw without fabricated data");
    auto& slow = hub.register_source("slow", "Telemetry", 10, {{"Value", "%", "v"}}, false);
    slow.online = true;
    slow.offset = 5;
    slow.data->append(0, std::vector<float>{1, 2, 3});
    f.frame();
    require(f.view->channel_visible(0) && f.view->points_this_frame() > 0,
        "Newly registered channels must display without UI selection");
    require(near(f.state().x.Max, 5.2), "Source offsets must map onto the common timeline");
    f.view->set_channel_visible(0, false);
    auto& timed = hub.register_source("timed", "Irregular", 0, {{"Value", "A", "v"}}, true);
    timed.online = true;
    timed.offset = 10;
    timed.data->append_received(0, std::vector<float>{7}, .1);
    timed.data->append_received(1, std::vector<float>{8}, .4);
    f.frame();
    require(!f.view->channel_visible(0) && f.view->channel_visible(1),
        "Adding channels must preserve existing host visibility choices");
    require(near(f.state().x.Max, 10.4), "Irregular receiver timestamps must use the same timeline");
    require(hub.label(0) != hub.label(1), "Duplicate channel names must have distinct labels");
    timed.online = false;
    f.frame();
    require(f.view->points_this_frame() > 0, "A disconnected source must retain its available history");
    f.view->set_channel_visible(0, true);
    f.frame();
    require(near(f.state().x.Max, 5.2), "Offline sources must not drive a live source's playhead");
    hub.clear();
    f.frame();
    require(f.view->points_this_frame() == 0, "Cleared data must display the empty state");
    std::cout << "PASS empty/cleared/disconnected states, dynamic channels and mixed source timelines\n";
}

void dense_queries_and_invalid_options() {
    Fixture f;
    f.append(20'000, 80'000);
    f.view->set_time_span(5);
    f.frame();
    const auto pixels = static_cast<std::size_t>(std::ceil(f.state().rect.GetWidth()));
    require(f.view->points_this_frame() > 0 && f.view->points_this_frame() <= (4 * pixels + 2) * 2,
        "Dense live rendering must submit a pixel-bounded number of samples");
    const auto reject = [](auto operation) {
        bool rejected = false;
        try { operation(); } catch (const std::exception&) { rejected = true; }
        require(rejected, "Invalid host options must be rejected");
    };
    reject([&] { f.view->set_time_span(0); });
    reject([&] { f.view->set_time_span(std::numeric_limits<double>::quiet_NaN()); });
    reject([&] { f.view->set_y_range(1, 1); });
    reject([&] { f.view->set_y_range(-std::numeric_limits<double>::max(), std::numeric_limits<double>::max()); });
    reject([&] { f.view->set_channel_visible(2, true); });
    reject([&] { (void)f.view->channel_visible(2); });
    reject([&] { wave::WaveView bad(f.data, {.time_span = -1}); });
    f.frame();
    require(near(f.state().x.Size(), 5) && near(f.state().y.Max, 10),
        "Rejected options must preserve the working viewport");
    std::cout << "PASS dense query limits and invalid host configuration\n";
}
} // namespace

int main() {
    try {
        steady_plot_geometry();
        fast_audio_narrow_view_has_stable_layout();
        sample_markers_follow_horizontal_zoom();
        irregular_sample_markers_respect_bursts();
        overlap_compensation_is_local_to_the_trace();
        default_view_fills_then_scrolls();
        default_span_uses_the_actual_frame_budget();
        irregular_full_ring_uses_real_elapsed_time();
        auto_y_tracks_visible_amplitudes();
        basic_display_and_host_api();
        zoom_can_be_disabled();
        direction_and_anchor();
        return_to_origin_and_event_independence();
        extreme_deltas_and_limits();
        zoom_has_no_minimum_scale();
        wheel_zoom_across_panel();
        two_finger_zoom();
#ifdef _WIN32
        win32_touchpad_contacts_drive_the_view();
#endif
        right_drag_matches_two_fingers_across_panel();
        unsupported_inputs_and_host_override();
        wheel_does_not_scroll_the_host();
        pause_keeps_snapshot_and_resumes_latest();
        pan_live_and_paused();
        resume_never_rewinds_to_a_delayed_clock();
        pause_input_scope_and_independent_views();
        embedding_is_local_and_independent();
        mixed_sources_and_empty_states();
        dense_queries_and_invalid_options();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
