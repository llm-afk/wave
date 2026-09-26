#include "wave_view.hpp"
#include "plot_navigation.hpp"
#include "overview_navigation.hpp"
#include "imgui.h"
#include "implot.h"
#include "implot_internal.h" // The pinned ImPlot version exposes legend hit bounds here.
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace wave {
namespace {
const std::array<ImVec4, 8> signal_colors = {{
    {0.94f, 0.20f, 0.18f, 1}, {0.25f, 0.65f, 0.98f, 1},
    {0.97f, 0.72f, 0.24f, 1}, {0.25f, 0.81f, 0.55f, 1},
    {0.78f, 0.47f, 0.94f, 1}, {0.30f, 0.82f, 0.86f, 1},
    {0.96f, 0.47f, 0.67f, 1}, {0.77f, 0.79f, 0.82f, 1}
}};

struct Gesture {
    bool held{};
    bool dragged{};
    ImVec2 press{};
    PlotRange initial{};
    ZoomAnchor anchor{};
    ZoomAxes axes{ZoomAxes::Both};
};
struct OverviewGesture {
    bool held{};
    bool following{};
    float press_x{}, pixel_width{};
    AxisRange initial{}, extent{};
    OverviewDrag kind{OverviewDrag::Move};
};
struct PlotWindow {
    int id{};
    bool open{true};
    bool follow{true};
    bool auto_y{true};
    bool markers{true};
    bool initialized{};
    bool overview_open{};
    double follow_span{0.2};
    PlotRange limits{{0, 0.2}, {-1, 1}};
    ImVec2 plot_pos{}, plot_size{};
    ImVec2 legend_pos{}, legend_size{};
    ImVec2 zoom_button_pos{}, zoom_button_size{};
    ImVec2 overview_pos{}, overview_size{};
    Gesture zoom, pan;
    OverviewGesture overview_drag;
    std::vector<bool> selected;
};
bool contains(ImVec2 p, ImVec2 pos, ImVec2 size) {
    return p.x >= pos.x && p.y >= pos.y && p.x < pos.x + size.x && p.y < pos.y + size.y;
}
bool over_overview(const PlotWindow& panel, ImVec2 mouse) {
    return contains(mouse, panel.zoom_button_pos, panel.zoom_button_size) ||
        (panel.overview_open && contains(mouse, panel.overview_pos, panel.overview_size));
}
ZoomAnchor anchor_at(const PlotWindow& panel, ImVec2 mouse) {
    const auto& r = panel.limits;
    const double x = std::clamp(double(mouse.x - panel.plot_pos.x) / std::max(1.0f, panel.plot_size.x), 0.0, 1.0);
    const double y = std::clamp(double(mouse.y - panel.plot_pos.y) / std::max(1.0f, panel.plot_size.y), 0.0, 1.0);
    return {r.x.min + x * (r.x.max - r.x.min), r.y.max - y * (r.y.max - r.y.min)};
}
} // namespace

void apply_style(float scale) {
    ImGui::StyleColorsDark();
    auto& s = ImGui::GetStyle();
    s.WindowPadding = {6, 5};
    s.FramePadding = {6, 3};
    s.ItemSpacing = {8, 5};
    s.WindowRounding = 0;
    s.ChildRounding = 0;
    s.FrameRounding = 2;
    s.PopupRounding = 3;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 0;
    s.FrameBorderSize = 0;
    s.Colors[ImGuiCol_Text] = {0.75f, 0.75f, 0.75f, 1};
    s.Colors[ImGuiCol_TextDisabled] = {0.43f, 0.43f, 0.43f, 1};
    s.Colors[ImGuiCol_WindowBg] = {0, 0, 0, 1};
    s.Colors[ImGuiCol_ChildBg] = {0, 0, 0, 1};
    s.Colors[ImGuiCol_PopupBg] = {0.075f, 0.075f, 0.075f, 1};
    s.Colors[ImGuiCol_MenuBarBg] = {0.085f, 0.085f, 0.085f, 1};
    s.Colors[ImGuiCol_Border] = {0.20f, 0.20f, 0.20f, 1};
    s.Colors[ImGuiCol_Separator] = {0.16f, 0.16f, 0.16f, 1};
    s.Colors[ImGuiCol_FrameBg] = {0.12f, 0.12f, 0.12f, 1};
    s.Colors[ImGuiCol_Button] = {0.13f, 0.13f, 0.13f, 1};
    s.Colors[ImGuiCol_ButtonHovered] = {0.22f, 0.22f, 0.22f, 1};
    s.Colors[ImGuiCol_ButtonActive] = {0.28f, 0.28f, 0.28f, 1};
    s.Colors[ImGuiCol_Header] = {0.19f, 0.19f, 0.19f, 1};
    s.Colors[ImGuiCol_HeaderHovered] = {0.25f, 0.25f, 0.25f, 1};
    s.Colors[ImGuiCol_HeaderActive] = {0.30f, 0.30f, 0.30f, 1};
    s.Colors[ImGuiCol_CheckMark] = {0.78f, 0.78f, 0.78f, 1};
    s.ScaleAllSizes(scale);
    s.FontScaleDpi = scale;
    auto& p = ImPlot::GetStyle();
    p.PlotPadding = {5 * scale, 4 * scale};
    p.LabelPadding = {3 * scale, 2 * scale};
    p.LegendPadding = {7 * scale, 5 * scale};
    p.LegendInnerPadding = {0, 0};
    p.FitPadding = {0, 0.1f};
    p.PlotBorderSize = 0;
    p.MinorAlpha = 0.35f;
    p.Colors[ImPlotCol_FrameBg] = {0, 0, 0, 1};
    p.Colors[ImPlotCol_PlotBg] = {0, 0, 0, 1};
    p.Colors[ImPlotCol_AxisGrid] = {0.18f, 0.18f, 0.18f, 1};
    p.Colors[ImPlotCol_AxisText] = {0.43f, 0.43f, 0.43f, 1};
    p.Colors[ImPlotCol_AxisTick] = {0.25f, 0.25f, 0.25f, 1};
    p.Colors[ImPlotCol_LegendBg] = {0, 0, 0, 0};
    p.Colors[ImPlotCol_LegendBorder] = {0, 0, 0, 0};
    p.Colors[ImPlotCol_LegendText] = {0.54f, 0.54f, 0.54f, 1};
    // Our view owns navigation. Keep every upstream mouse index valid.
    ImPlot::GetInputMap().Menu = ImGuiMouseButton_Right;
}

// Only the data store/snapshot interface crosses into the view. Producers do
// not know about the window, its frame rate, or any navigation state.
struct WaveView::Impl {
    explicit Impl(DataStore& store) : store_(store) { add_window({0}); }

    void draw() {
        const auto live_range = store_.range();
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && !ImGui::GetIO().WantTextInput && !any_popup())
            toggle_pause(live_range);
        draw_menu();
        points_this_frame_ = 0;
        const auto* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0, 0});
        ImGui::Begin("##canvas", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus);
        const float height = std::max(1.0f, (ImGui::GetContentRegionAvail().y - static_cast<float>(panels_.size() - 1)) / static_cast<float>(panels_.size()));
        for (auto& panel : panels_) {
            ImGui::PushID(panel.id);
            if (ImGui::BeginChild("panel", {0, height}, ImGuiChildFlags_None,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) draw_panel(panel);
            ImGui::EndChild();
            ImGui::PopID();
            if (&panel != &panels_.back()) ImGui::Separator();
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
        std::erase_if(panels_, [](const auto& panel) { return !panel.open; });
        if (panels_.empty()) add_window({});
        if (!active_panel()) active_id_ = panels_.front().id;
        if (show_performance_) draw_performance(live_range);
    }
    std::size_t points_this_frame() const { return points_this_frame_; }

private:
    DataStore& store_;
    FrozenData frozen_;
    std::vector<PlotWindow> panels_;
    bool paused_{};
    bool grid_{};
    bool show_performance_{};
    int next_id_{1}, active_id_{1};
    std::size_t points_this_frame_{};

    static bool any_popup() {
        return ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    }
    PlotWindow* active_panel() {
        for (auto& panel : panels_) if (panel.id == active_id_) return &panel;
        return nullptr;
    }
    void add_window(std::initializer_list<std::size_t> channels) {
        PlotWindow panel;
        panel.id = next_id_++;
        panel.selected.resize(store_.channels().size(), false);
        for (auto channel : channels) if (channel < panel.selected.size()) panel.selected[channel] = true;
        active_id_ = panel.id;
        panels_.push_back(std::move(panel));
    }
    void toggle_pause(DataRange range) {
        if (!paused_) {
            if (range.empty) return;
            auto snapshot = store_.snapshot(range.begin, range.end);
            if (snapshot.range().empty) return;
            frozen_ = std::move(snapshot);
            paused_ = true;
            for (auto& panel : panels_) {
                panel.auto_y = false;
                panel.zoom = {};
                panel.pan = {};
                panel.overview_drag = {};
            }
        } else {
            resume_updates();
        }
    }
    void resume_updates() {
        paused_ = false;
        frozen_ = {};
        for (auto& panel : panels_) {
            if (panel.initialized) panel.follow_span = panel.limits.x.max - panel.limits.x.min;
            panel.auto_y = false;
            panel.zoom = {};
            panel.pan = {};
            panel.overview_drag = {};
        }
    }
    static void align_to_latest(PlotWindow& panel, DataRange range) {
        const double right = range.empty ? panel.follow_span : range.end;
        panel.limits.x = {right - panel.follow_span, right};
    }
    void reset_view(PlotWindow& panel) {
        panel.follow = true;
        panel.auto_y = true;
        panel.zoom = {};
        panel.pan = {};
        panel.overview_drag = {};
        align_to_latest(panel, paused_ ? frozen_.range() : store_.range());
    }
    void signal_selector(PlotWindow& panel) {
        const auto& channels = store_.channels();
        for (std::size_t i = 0; i < channels.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::PushStyleColor(ImGuiCol_Text, signal_colors[i % signal_colors.size()]);
            bool selected = panel.selected[i];
            if (ImGui::Checkbox(channels[i].name.c_str(), &selected)) panel.selected[i] = selected;
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("清除信号")) std::fill(panel.selected.begin(), panel.selected.end(), false);
    }
    void view_options(PlotWindow& panel) {
        ImGui::MenuItem("点 + 线", nullptr, &panel.markers);
        ImGui::MenuItem("Y 轴自适应", nullptr, &panel.auto_y);
        if (ImGui::MenuItem("复位视图")) reset_view(panel);
        if (ImGui::MenuItem("查看整个缓存")) {
            const auto range = paused_ ? frozen_.range() : store_.range();
            if (!range.empty) {
                panel.limits.x = {range.begin, std::max(range.end, range.begin + 8 / store_.sample_rate())};
                panel.follow = false;
                panel.auto_y = true;
            }
        }
        if (panels_.size() > 1 && ImGui::MenuItem("关闭此窗口")) panel.open = false;
    }
    void draw_menu() {
        if (!ImGui::BeginMainMenuBar()) return;
        if (ImGui::MenuItem("+ 窗口", nullptr, false, panels_.size() < 6)) add_window({});
        auto* panel = active_panel();
        if (ImGui::BeginMenu("信号", panel != nullptr)) {
            signal_selector(*panel);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("视图")) {
            if (ImGui::BeginMenu("时间跨度", panel != nullptr)) {
                const double spans[] = {0.001, 0.01, 0.05, 0.2, 1, 5, 30};
                const char* names[] = {"1 ms", "10 ms", "50 ms", "200 ms", "1 s", "5 s", "30 s"};
                for (int i = 0; i < 7; ++i) if (ImGui::MenuItem(names[i], nullptr, panel->follow_span == spans[i])) {
                    panel->follow_span = spans[i];
                    panel->follow = true;
                    align_to_latest(*panel, paused_ ? frozen_.range() : store_.range());
                }
                ImGui::EndMenu();
            }
            if (panel) view_options(*panel);
            ImGui::Separator();
            ImGui::MenuItem("网格", nullptr, &grid_);
            ImGui::MenuItem("性能信息", nullptr, &show_performance_);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("?")) {
            ImGui::TextUnformatted("右键拖动：向右放大 X，向上放大 Y；反向缩小");
            ImGui::TextUnformatted("Shift + 右拖：仅 X 轴；Ctrl + 右拖：仅 Y 轴");
            ImGui::TextUnformatted("右键单击：添加信号及设置；左键拖动：跟踪时平移 Y，冻结后平移两轴");
            ImGui::TextUnformatted("滚轮：时间轴缩放；靠近 Y 轴或 Ctrl + 滚轮：Y 轴缩放");
            ImGui::TextUnformatted("左键平移、右键及滚轮缩放均保持实时跟随");
            ImGui::TextUnformatted("双击 / Space：冻结或恢复整个显示缓冲区，后台接收继续");
            ImGui::TextUnformatted("冻结后可自由平移缩放；恢复时保留各窗口跟踪模式和两轴尺度");
            ImGui::TextUnformatted("图例：隐藏信号；复位视图：右键菜单或视图菜单");
            ImGui::TextUnformatted("Zoom：缓存概览；冻结后移动选框或拖边缘，跟踪时只调整时间跨度");
            ImGui::Separator();
            ImGui::TextUnformatted("X 轴单位：秒。冻结保留整段缓存，可自由检查所有通道。");
            ImGui::TextUnformatted("密集数据保留极值及采样点，放大后可见各个点。");
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
    DragZoomLimits zoom_limits() const {
        DragZoomLimits result;
        result.sample_rate = store_.sample_rate();
        result.max_x_span = std::max(3600.0, static_cast<double>(store_.capacity_frames()) / store_.sample_rate() * 2);
        result.pixels_per_e_fold = default_zoom_pixels_per_e_fold * ImGui::GetStyle().FontScaleDpi;
        return result;
    }
    void navigate(PlotWindow& panel) {
        auto& io = ImGui::GetIO();
        if (!panel.initialized || io.AppFocusLost || any_popup()) {
            panel.zoom = {};
            panel.pan = {};
            panel.overview_drag = {};
            return;
        }
        if (panel.overview_drag.held) return;
        const bool window_hovered = ImGui::IsWindowHovered() && !over_overview(panel, io.MousePos);
        const bool hovered = window_hovered && contains(io.MousePos, panel.plot_pos, panel.plot_size);
        // Include the Y tick labels and a small strip inside the plotting area.
        const bool near_y_axis = window_hovered && io.MousePos.y >= panel.plot_pos.y &&
            io.MousePos.y < panel.plot_pos.y + panel.plot_size.y &&
            io.MousePos.x < panel.plot_pos.x + 24 * ImGui::GetStyle().FontScaleDpi;
        const bool over_legend = contains(io.MousePos, panel.legend_pos, panel.legend_size);
        if ((hovered && (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1))) ||
            ((hovered || near_y_axis) && io.MouseWheel != 0)) active_id_ = panel.id;
        if (hovered && !over_legend && ImGui::IsMouseDoubleClicked(0)) {
            toggle_pause(store_.range());
            return;
        }
        if (hovered && ImGui::IsMouseClicked(1) && !panel.pan.held) {
            panel.zoom = {true, false, io.MousePos, panel.limits, anchor_at(panel, io.MousePos),
                io.KeyShift ? ZoomAxes::XOnly : io.KeyCtrl ? ZoomAxes::YOnly : ZoomAxes::Both};
            if (panel.follow && !paused_) panel.zoom.anchor.x = panel.limits.x.max;
        }
        if (panel.zoom.held) {
            auto& gesture = panel.zoom;
            const double dx = io.MousePos.x - gesture.press.x, dy = io.MousePos.y - gesture.press.y;
            const double threshold = 3 * ImGui::GetStyle().FontScaleDpi;
            gesture.dragged |= dx * dx + dy * dy >= threshold * threshold;
            if (gesture.dragged) {
                if (gesture.axes != ZoomAxes::XOnly) panel.auto_y = false;
                panel.limits = zoom_from_drag(gesture.initial, gesture.anchor, dx, dy, gesture.axes, zoom_limits());
                if (gesture.axes != ZoomAxes::YOnly)
                    panel.follow_span = panel.limits.x.max - panel.limits.x.min;
            }
            if (ImGui::IsMouseReleased(1) || !ImGui::IsMouseDown(1)) {
                // A latched drag flag matters: returning to the press position
                // must not turn a completed zoom gesture into a context click.
                if (!gesture.dragged && hovered) ImGui::OpenPopup("plot_context");
                gesture = {};
            }
            return;
        }
        if (hovered && !over_legend && ImGui::IsMouseClicked(0))
            panel.pan = {true, false, io.MousePos, panel.limits, {}, ZoomAxes::Both};
        if (panel.pan.held) {
            auto& gesture = panel.pan;
            const double dx = io.MousePos.x - gesture.press.x, dy = io.MousePos.y - gesture.press.y;
            const double threshold = 4 * ImGui::GetStyle().FontScaleDpi;
            gesture.dragged |= dx * dx + dy * dy >= threshold * threshold;
            if (gesture.dragged) {
                panel.auto_y = false;
                const auto& initial = gesture.initial;
                const double ty = dy / std::max(1.0f, panel.plot_size.y) * (initial.y.max - initial.y.min);
                panel.limits.y = {initial.y.min + ty, initial.y.max + ty};
                if (!panel.follow || paused_) {
                    const double tx = -dx / std::max(1.0f, panel.plot_size.x) * (initial.x.max - initial.x.min);
                    panel.limits.x = {initial.x.min + tx, initial.x.max + tx};
                }
            }
            if (ImGui::IsMouseReleased(0) || !ImGui::IsMouseDown(0)) gesture = {};
            return;
        }
        if ((hovered || near_y_axis) && io.MouseWheel != 0) {
            const double delta = std::clamp(double(io.MouseWheel), -8.0, 8.0) * 35 * ImGui::GetStyle().FontScaleDpi;
            const bool zoom_y = near_y_axis || io.KeyCtrl;
            auto anchor = anchor_at(panel, io.MousePos);
            if (!zoom_y && panel.follow && !paused_) anchor.x = panel.limits.x.max;
            panel.limits = zoom_from_drag(panel.limits, anchor, delta, -delta,
                zoom_y ? ZoomAxes::YOnly : ZoomAxes::XOnly, zoom_limits());
            if (zoom_y) panel.auto_y = false;
            else panel.follow_span = panel.limits.x.max - panel.limits.x.min;
        }
    }
    void draw_overview(PlotWindow& panel, DataRange range) {
        const float scale = ImGui::GetStyle().FontScaleDpi;
        const auto saved_cursor = ImGui::GetCursorScreenPos();
        panel.zoom_button_size = {42 * scale, 20 * scale};
        panel.zoom_button_pos = {panel.plot_pos.x + panel.plot_size.x - 48 * scale, panel.plot_pos.y + 2 * scale};
        panel.overview_size = {std::min(320 * scale, panel.plot_size.x - 12 * scale),
            std::min(100 * scale, panel.plot_size.y - 32 * scale)};
        panel.overview_pos = {panel.plot_pos.x + panel.plot_size.x - panel.overview_size.x - 6 * scale,
            panel.plot_pos.y + 26 * scale};
        ImGui::SetCursorScreenPos(panel.zoom_button_pos);
        ImGui::PushStyleColor(ImGuiCol_Button, panel.overview_open ? ImVec4{0.13f, 0.15f, 0.18f, 1} : ImVec4{0.025f, 0.025f, 0.025f, 1});
        ImGui::BeginDisabled(panel.zoom.held || panel.pan.held);
        if (ImGui::Button("Zoom", panel.zoom_button_size)) {
            panel.overview_open = !panel.overview_open;
            panel.overview_drag = {};
            active_id_ = panel.id;
        }
        ImGui::EndDisabled();
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("展开 / 收起整段缓存概览。\n跟踪时调整时间跨度；双击主波形冻结后，可移动选框或拖动两边。\n仅改变当前窗口的时间轴。");
        if (panel.overview_open && panel.overview_size.x >= 80 * scale && panel.overview_size.y >= 36 * scale) {
            ImGui::SetCursorScreenPos(panel.overview_pos);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {6 * scale, 6 * scale});
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0, 3 * scale});
            ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, scale);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.018f, 0.018f, 0.018f, 1});
            if (ImGui::BeginChild("zoom_overview", panel.overview_size, ImGuiChildFlags_Borders,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings)) {
                const auto graph_pos = ImGui::GetCursorScreenPos();
                const ImVec2 graph_size{ImGui::GetContentRegionAvail().x,
                    std::max(8 * scale, ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeight() - 3 * scale)};
                ImGui::InvisibleButton("##overview_canvas", graph_size);
                const bool hovered = ImGui::IsItemHovered();
                auto& io = ImGui::GetIO();
                auto* draw = ImGui::GetWindowDrawList();
                const ImVec2 graph_end{graph_pos.x + graph_size.x, graph_pos.y + graph_size.y};
                draw->AddRectFilled(graph_pos, graph_end, IM_COL32(9, 9, 9, 255));
                const AxisRange extent{range.begin, range.end};
                const bool usable = !range.empty && extent.max > extent.min && graph_size.x > 0;
                if (usable) {
                    const double duration = extent.max - extent.min;
                    const double minimum = std::min(8 / store_.sample_rate(), duration);
                    const bool following = panel.follow && !paused_;
                    auto to_pixel = [&](double t) {
                        return graph_pos.x + static_cast<float>(std::clamp((t - extent.min) / duration, 0.0, 1.0)) * graph_size.x;
                    };
                    auto selection_pixels = [&]() {
                        const float left = to_pixel(panel.limits.x.min), right = to_pixel(panel.limits.x.max);
                        const float width = std::min(graph_size.x, std::max(18 * scale, right - left));
                        const float x = std::clamp((left + right - width) * 0.5f, graph_pos.x, graph_end.x - width);
                        return ImVec2{x, x + width};
                    };
                    auto apply_range = [&](AxisRange value) {
                        panel.limits.x = value;
                        panel.follow_span = value.max - value.min;
                        if (following) align_to_latest(panel, range);
                    };
                    auto edges = selection_pixels();
                    const float grip = 5 * scale;
                    const bool at_left = std::abs(io.MousePos.x - edges.x) <= grip;
                    const bool at_right = std::abs(io.MousePos.x - edges.y) <= grip;
                    const bool in_box = io.MousePos.x >= edges.x && io.MousePos.x <= edges.y;
                    const bool input_allowed = !io.AppFocusLost && !any_popup() && !panel.zoom.held && !panel.pan.held;
                    if (!input_allowed) panel.overview_drag = {};
                    if (hovered && input_allowed) {
                        ImGui::SetMouseCursor(following || at_left || at_right ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeAll);
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                            active_id_ = panel.id;
                            auto kind = following || at_left ? OverviewDrag::ResizeMin : at_right ? OverviewDrag::ResizeMax : OverviewDrag::Move;
                            if (!in_box && !at_left && !at_right) {
                                const double t = extent.min + std::clamp(double(io.MousePos.x - graph_pos.x) / graph_size.x, 0.0, 1.0) * duration;
                                const double span = std::clamp(panel.limits.x.max - panel.limits.x.min, minimum, duration);
                                const AxisRange selected = following ? AxisRange{std::min(t, extent.max - minimum), extent.max}
                                    : AxisRange{t - span * 0.5, t + span * 0.5};
                                apply_range(overview_range_from_drag(selected, extent, 0, OverviewDrag::Move, minimum, following));
                            }
                            panel.overview_drag = {true, following, io.MousePos.x, graph_size.x, panel.limits.x, extent, kind};
                        }
                    }
                    if (panel.overview_drag.held) {
                        const auto& gesture = panel.overview_drag;
                        const double delta = double(io.MousePos.x - gesture.press_x) / std::max(1.0f, gesture.pixel_width)
                            * (gesture.extent.max - gesture.extent.min);
                        apply_range(overview_range_from_drag(gesture.initial, gesture.extent, delta,
                            gesture.kind, minimum, gesture.following));
                        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) panel.overview_drag = {};
                    } else if (hovered && input_allowed && io.MouseWheel != 0) {
                        const double cursor_t = extent.min + std::clamp(double(io.MousePos.x - graph_pos.x) / graph_size.x, 0.0, 1.0) * duration;
                        const double anchor = following ? panel.limits.x.max : std::clamp(cursor_t, panel.limits.x.min, panel.limits.x.max);
                        const double delta = std::clamp(double(io.MouseWheel), -8.0, 8.0) * 35 * scale;
                        const auto zoomed = zoom_from_drag(panel.limits, {anchor, 0}, delta, 0, ZoomAxes::XOnly, zoom_limits());
                        apply_range(overview_range_from_drag(zoomed.x, extent, 0, OverviewDrag::Move, minimum, following));
                        active_id_ = panel.id;
                    }

                    draw->PushClipRect(graph_pos, graph_end, true);
                    const auto selected = std::find(panel.selected.begin(), panel.selected.end(), true);
                    if (selected != panel.selected.end()) {
                        const auto channel = static_cast<std::size_t>(selected - panel.selected.begin());
                        const auto width = static_cast<std::size_t>(std::max(1.0f, graph_size.x));
                        const auto points = paused_ ? frozen_.query(channel, extent.min, extent.max, width)
                            : store_.query(channel, extent.min, extent.max, width);
                        if (!points.empty()) {
                            const auto [lo, hi] = std::minmax_element(points.begin(), points.end(),
                                [](const auto& a, const auto& b) { return a.value < b.value; });
                            const double middle = (lo->value + hi->value) * 0.5;
                            const double y_span = std::max(1e-9, double(hi->value - lo->value)) * 1.2;
                            std::vector<ImVec2> preview;
                            preview.reserve(points.size());
                            for (const auto& point : points) preview.push_back({to_pixel(point.t),
                                graph_pos.y + graph_size.y * static_cast<float>(0.5 - (point.value - middle) / y_span)});
                            auto color = signal_colors[channel % signal_colors.size()];
                            color.w = 0.55f;
                            draw->AddPolyline(preview.data(), static_cast<int>(preview.size()), ImGui::GetColorU32(color), ImDrawFlags_None, scale);
                            points_this_frame_ += points.size();
                        }
                    }
                    edges = selection_pixels();
                    // Keep a narrow true-range mark separate from the larger
                    // handles so a subpixel view stays easy to grab without
                    // turning its hit width into a minimum data span.
                    const float true_left = to_pixel(panel.limits.x.min), true_right = to_pixel(panel.limits.x.max);
                    draw->AddRectFilled({true_left, graph_pos.y}, {std::max(true_left + scale, true_right), graph_end.y}, IM_COL32(126, 172, 205, 42));
                    draw->AddRect({edges.x, graph_pos.y + scale}, {edges.y, graph_end.y - scale}, IM_COL32(151, 184, 206, 220), 0, ImDrawFlags_None, scale);
                    draw->AddRectFilled({edges.x, graph_pos.y + 3 * scale}, {edges.x + 3 * scale, graph_end.y - 3 * scale}, IM_COL32(173, 199, 218, 230));
                    if (!following) draw->AddRectFilled({edges.y - 3 * scale, graph_pos.y + 3 * scale}, {edges.y, graph_end.y - 3 * scale}, IM_COL32(173, 199, 218, 230));
                    draw->PopClipRect();
                } else {
                    panel.overview_drag = {};
                    draw->AddText({graph_pos.x + 4 * scale, graph_pos.y + 4 * scale}, IM_COL32(100, 100, 100, 255), "暂无数据");
                }
                ImGui::TextDisabled("%.3f s", range.empty ? 0.0 : range.begin);
                char end_label[48];
                ImFormatString(end_label, IM_ARRAYSIZE(end_label), "%.3f s", range.empty ? 0.0 : range.end);
                ImGui::SameLine(std::max(0.0f, panel.overview_size.x - 6 * scale - ImGui::CalcTextSize(end_label).x));
                ImGui::TextDisabled("%s", end_label);
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(3);
        } else {
            panel.overview_drag = {};
        }
        ImGui::SetCursorScreenPos(saved_cursor);
    }
    void draw_panel(PlotWindow& panel) {
        navigate(panel);
        const auto range = paused_ ? frozen_.range() : store_.range();
        if (!panel.initialized || (panel.follow && !paused_))
            align_to_latest(panel, range);
        const bool has_signals = std::any_of(panel.selected.begin(), panel.selected.end(), [](bool b) { return b; });
        constexpr auto flags = ImPlotFlags_NoTitle | ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect |
            ImPlotFlags_NoMouseText | ImPlotFlags_NoInputs;
        if (ImPlot::BeginPlot("##wave", {-1, -1}, flags)) {
            const ImPlotAxisFlags axis = ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoHighlight |
                ImPlotAxisFlags_NoMenus | ImPlotAxisFlags_NoSideSwitch | ImPlotAxisFlags_NoInitialFit |
                (grid_ ? 0 : ImPlotAxisFlags_NoGridLines);
            ImPlot::SetupAxes(nullptr, nullptr, axis, axis | (panel.auto_y ? ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_RangeFit : 0));
            ImPlot::SetupAxisLinks(ImAxis_X1, &panel.limits.x.min, &panel.limits.x.max);
            ImPlot::SetupAxisLinks(ImAxis_Y1, &panel.limits.y.min, &panel.limits.y.max);
            ImPlot::SetupLegend(ImPlotLocation_NorthWest, ImPlotLegendFlags_Horizontal |
                (over_overview(panel, ImGui::GetIO().MousePos) ? ImPlotLegendFlags_NoButtons : 0));
            ImPlot::SetupFinish();
            panel.initialized = true;
            panel.plot_pos = ImPlot::GetPlotPos();
            panel.plot_size = ImPlot::GetPlotSize();
            const auto limits = ImPlot::GetPlotLimits();
            const auto width = static_cast<std::size_t>(std::max(1.0f, panel.plot_size.x));
            const double samples = (limits.X.Max - limits.X.Min) * store_.sample_rate();
            const float marker_size = samples < width / 3.0 ? 1.28f : samples < width ? 0.8f : 0.38f;
            bool any_points = false;
            for (std::size_t i = 0; i < panel.selected.size(); ++i) {
                if (!panel.selected[i]) continue;
                const auto points = paused_ ? frozen_.query(i, limits.X.Min, limits.X.Max, width)
                    : store_.query(i, limits.X.Min, limits.X.Max, width);
                if (points.empty()) continue;
                ImPlotSpec spec;
                const auto color = signal_colors[i % signal_colors.size()];
                spec.LineColor = {color.x, color.y, color.z, 0.75f};
                spec.LineWeight = 0.85f;
                spec.Marker = panel.markers ? ImPlotMarker_Circle : ImPlotMarker_None;
                spec.MarkerSize = marker_size * ImGui::GetStyle().FontScaleDpi;
                spec.MarkerFillColor = color;
                spec.MarkerLineColor = {0, 0, 0, 0};
                spec.Stride = sizeof(PlotPoint);
                const auto& channel = store_.channels()[i];
                const std::string label = channel.name + (channel.unit.empty() ? "" : " [" + channel.unit + "]")
                    + "###channel" + std::to_string(i);
                ImPlot::PlotLine(label.c_str(), &points[0].t, &points[0].value, static_cast<int>(points.size()), spec);
                any_points = true;
                points_this_frame_ += points.size();
            }
            const auto pos = panel.plot_pos, size = panel.plot_size;
            auto* draw_list = ImPlot::GetPlotDrawList();
            draw_list->AddLine(pos, {pos.x, pos.y + size.y}, IM_COL32(56, 56, 56, 255));
            draw_list->AddLine({pos.x, pos.y + size.y}, {pos.x + size.x, pos.y + size.y}, IM_COL32(56, 56, 56, 255));
            if (!has_signals || (!range.empty && !any_points)) {
                const char* hint = !has_signals ? "右键添加信号" : paused_
                    ? "此区间不在冻结缓存内" : "此区间已超出缓存，可在右键菜单中复位视图";
                const auto text_size = ImGui::CalcTextSize(hint);
                draw_list->AddText({pos.x + (size.x - text_size.x) / 2, pos.y + size.y / 2},
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), hint);
            }
            if (panels_.size() > 1) {
                const std::string label = std::to_string(panel.id);
                const auto text_size = ImGui::CalcTextSize(label.c_str());
                const ImVec2 label_pos{pos.x + size.x - text_size.x - 58 * ImGui::GetStyle().FontScaleDpi, pos.y + 5};
                draw_list->AddText(label_pos, IM_COL32(110, 110, 110, 255), label.c_str());
            }
            auto* plot = ImPlot::GetCurrentPlot();
            ImPlot::EndPlot();
            panel.legend_pos = plot->Items.Legend.RectClamped.Min;
            panel.legend_size = plot->Items.Legend.RectClamped.GetSize();
            draw_overview(panel, range);
        }
        if (ImGui::BeginPopup("plot_context")) {
            signal_selector(panel);
            ImGui::Separator();
            view_options(panel);
            ImGui::EndPopup();
        }
    }
    void draw_performance(DataRange range) {
        ImGui::SetNextWindowSize({280 * ImGui::GetStyle().FontScaleDpi, 0}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("性能信息", &show_performance_, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("%.0f kHz / 通道 · %zu 通道", store_.sample_rate() / 1000, store_.channels().size());
            const auto displayed_range = paused_ ? frozen_.range() : range;
            ImGui::Text("显示缓存 %zu / %zu 点/通道", displayed_range.frame_count, store_.capacity_frames());
            if (paused_) ImGui::TextDisabled("已冻结；后台接收继续");
            ImGui::Text("%.0f FPS · %zu 绘制点", ImGui::GetIO().Framerate, points_this_frame_);
        }
        ImGui::End();
    }
};

WaveView::WaveView(DataStore& store) : impl_(std::make_unique<Impl>(store)) {}
WaveView::~WaveView() = default;
void WaveView::draw() { impl_->draw(); }
std::size_t WaveView::points_this_frame() const { return impl_->points_this_frame(); }
} // namespace wave
