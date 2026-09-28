#include "wave_view.hpp"
#include "plot_navigation.hpp"
#include "overview_navigation.hpp"
#include "display_clock.hpp"
#include "signal_hub.hpp"
#include "imgui.h"
#include "implot.h"
#include "implot_internal.h" // The pinned ImPlot version exposes legend hit bounds here.
#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
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
    std::vector<bool> legend_hidden;
    struct BoxSelection {
        bool held{};
        ImVec2 start{}, end{}, origin{}, size{};
        PlotRange initial{};
    } box;
    int id{};
    bool open{true};
    bool follow{true};
    bool auto_y{true};
    bool markers{true};
    bool initialized{};
    bool x_range_initialized{};
    bool overview_open{};
    std::vector<std::uint64_t> first_visible_frames;
    double follow_span{0.2};
    PlotRange limits{{0, 0.2}, {-1, 1}};
    ImVec2 plot_pos{}, plot_size{};
    ImVec2 frame_pos{}, frame_size{};
    ImVec2 legend_pos{}, legend_size{};
    ImVec2 overview_pos{}, overview_size{};
    Gesture zoom, pan;
    OverviewGesture overview_drag;
    std::vector<bool> selected;
};
bool contains(ImVec2 p, ImVec2 pos, ImVec2 size) {
    return p.x >= pos.x && p.y >= pos.y && p.x < pos.x + size.x && p.y < pos.y + size.y;
}
bool over_overview(const PlotWindow& panel, ImVec2 mouse) {
    return panel.overview_open && contains(mouse, panel.overview_pos, panel.overview_size);
}
ZoomAnchor anchor_at(const PlotWindow& panel, ImVec2 mouse) {
    const auto& r = panel.limits;
    const double x = std::clamp(double(mouse.x - panel.plot_pos.x) / std::max(1.0f, panel.plot_size.x), 0.0, 1.0);
    const double y = std::clamp(double(mouse.y - panel.plot_pos.y) / std::max(1.0f, panel.plot_size.y), 0.0, 1.0);
    return {r.x.min + x * (r.x.max - r.x.min), r.y.max - y * (r.y.max - r.y.min)};
}
} // namespace

void apply_style(float scale) {
    ImGui::GetStyle() = ImGuiStyle{};
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
    s.Colors[ImGuiCol_Text] = {0.90f, 0.90f, 0.90f, 1};
    s.Colors[ImGuiCol_TextDisabled] = {0.57f, 0.57f, 0.57f, 1};
    s.Colors[ImGuiCol_WindowBg] = {0, 0, 0, 1};
    s.Colors[ImGuiCol_ChildBg] = {0, 0, 0, 1};
    s.Colors[ImGuiCol_PopupBg] = {0.075f, 0.075f, 0.075f, 1};
    s.Colors[ImGuiCol_MenuBarBg] = {0.085f, 0.085f, 0.085f, 1};
    s.Colors[ImGuiCol_Border] = {0.20f, 0.20f, 0.20f, 1};
    s.Colors[ImGuiCol_Separator] = {0.16f, 0.16f, 0.16f, 1};
    s.Colors[ImGuiCol_FrameBg] = {0.15f, 0.15f, 0.15f, 1};
    s.Colors[ImGuiCol_Button] = {0.13f, 0.13f, 0.13f, 1};
    s.Colors[ImGuiCol_ButtonHovered] = {0.22f, 0.22f, 0.22f, 1};
    s.Colors[ImGuiCol_ButtonActive] = {0.28f, 0.28f, 0.28f, 1};
    s.Colors[ImGuiCol_Header] = {0.19f, 0.19f, 0.19f, 1};
    s.Colors[ImGuiCol_HeaderHovered] = {0.25f, 0.25f, 0.25f, 1};
    s.Colors[ImGuiCol_HeaderActive] = {0.30f, 0.30f, 0.30f, 1};
    s.Colors[ImGuiCol_CheckMark] = {0.92f, 0.92f, 0.92f, 1};
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
    p.Colors[ImPlotCol_AxisText] = {0.80f, 0.80f, 0.80f, 1};
    p.Colors[ImPlotCol_AxisTick] = {0.56f, 0.56f, 0.56f, 1};
    p.Colors[ImPlotCol_LegendBg] = {0, 0, 0, 0};
    p.Colors[ImPlotCol_LegendBorder] = {0, 0, 0, 0};
    p.Colors[ImPlotCol_LegendText] = {0.78f, 0.78f, 0.78f, 1};
    // Our view owns navigation. Keep every upstream mouse index valid.
    ImPlot::GetInputMap().Menu = ImGuiMouseButton_Right;
}

// Only the data store/snapshot interface crosses into the view. Producers do
// not know about the window, its frame rate, or any navigation state.
struct WaveView::Impl {
    bool source_connected_=true;
    // Receiver-timed data has no nominal rate; allow microsecond navigation.
    double navigation_rate() const { return store_.reception_time() ? 1000000.0 : store_.sample_rate(); }
    double initial_span_;
    explicit Impl(DataStore& store, double span) : initial_span_(span), owned_store_(store), store_(owned_store_) { add_window({}); }
    explicit Impl(SignalHub& store, double span) : initial_span_(span), store_(store) { add_window({}); }

    void draw() {
        store_.sync();
        for (auto& panel : panels_) panel.selected.resize(store_.channels().size(), false);
        const auto live_range = store_.range();
        store_.update_clocks(ImGui::GetTime());
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
        const float height = std::max(1.0f, ImGui::GetContentRegionAvail().y / static_cast<float>(std::max(std::size_t{1}, panels_.size())));
        int linked_source = 0;
        AxisRange linked_range{};
        for (auto& panel : panels_) {
            const auto previous_x = panel.limits.x;
            const double previous_span = panel.follow_span;
            const bool can_link = link_x_ && panel.initialized;
            ImGui::PushID(panel.id);
            if (ImGui::BeginChild("panel", {0, height}, ImGuiChildFlags_None,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) draw_panel(panel);
            ImGui::EndChild();
            ImGui::PopID();
            // Live arrival moves the time range without a user gesture. Link
            // only span edits live; while frozen, link translations as well.
            const bool x_edited = paused_
                ? panel.limits.x.min != previous_x.min || panel.limits.x.max != previous_x.max
                : panel.follow_span != previous_span;
            if (can_link && active_id_ == panel.id && x_edited) {
                linked_source = panel.id;
                linked_range = panel.limits.x;
            }
        }
        if (linked_source) {
            for (auto& panel : panels_) {
                panel.limits.x = linked_range;
                panel.follow_span = linked_range.max - linked_range.min;
                if (panel.id != linked_source) {
                    panel.zoom = {};
                    panel.pan = {}; panel.box = {};
                    panel.overview_drag = {};
                }
            }
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
        std::erase_if(panels_, [](const auto& panel) { return !panel.open; });
        if (!active_panel()) active_id_ = panels_.empty() ? 0 : panels_.front().id;
        if (show_performance_) draw_performance(live_range);
    }
    std::size_t points_this_frame() const { return points_this_frame_; }

private:
    SignalHub owned_store_;
    SignalHub& store_;
    SignalSnapshot frozen_;
    std::vector<PlotWindow> panels_;
    bool paused_{};
    bool link_x_{true};
    bool grid_{};
    bool show_performance_{};
    int next_id_{1}, active_id_{1};
    std::size_t points_this_frame_{};
    int buffer_points_{100000};
    std::string buffer_error_;

    static bool any_popup() {
        return ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    }
    PlotWindow* active_panel() {
        for (auto& panel : panels_) if (panel.id == active_id_) return &panel;
        return nullptr;
    }
    void add_window(std::initializer_list<std::size_t> channels) {
        PlotWindow panel;
        panel.follow_span = initial_span_;
        panel.limits.x = {0, initial_span_};
        panel.id = next_id_++;
        panel.selected.resize(store_.channels().size(), false);
        for (auto channel : channels) if (channel < panel.selected.size()) panel.selected[channel] = true;
        if (link_x_) {
            const auto* source = active_panel();
            if (!source && !panels_.empty()) source = &panels_.front();
            if (source && source->initialized) {
                panel.limits.x = source->limits.x;
                panel.follow_span = source->limits.x.max - source->limits.x.min;
                panel.follow = source->follow;
                panel.x_range_initialized = true;
            }
        }
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
                panel.zoom = {};
                panel.pan = {}; panel.box = {};
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
            panel.zoom = {};
            panel.pan = {}; panel.box = {};
            panel.overview_drag = {};
        }
    }
    void clear_buffer() {
        store_.clear();
        frozen_ = {};
        for (auto& panel : panels_) {
            panel.first_visible_frames.clear();
            panel.zoom = {};
            panel.pan = {}; panel.box = {};
            panel.overview_drag = {};
        }
    }
    void clear_window_data(PlotWindow& panel) {
        panel.first_visible_frames.resize(store_.channels().size(),0);
        for(std::size_t i=0;i<panel.first_visible_frames.size();++i) {
            const auto live=store_.channel_range(i), frozen=frozen_.channel_range(i);
            if(!live.empty) panel.first_visible_frames[i]=std::max(panel.first_visible_frames[i],live.last_frame+1);
            if(!frozen.empty) panel.first_visible_frames[i]=std::max(panel.first_visible_frames[i],frozen.last_frame+1);
        }
        panel.zoom = {};
        panel.pan = {}; panel.box = {};
        panel.overview_drag = {};
    }
    DataRange visible_range(const PlotWindow& panel) const {
        DataRange range;
        const bool selected=std::any_of(panel.selected.begin(),panel.selected.end(),[](bool v){return v;});
        for(std::size_t i=0;i<panel.selected.size();++i) if(!selected || panel.selected[i]) {
            const auto cutoff=i<panel.first_visible_frames.size()?panel.first_visible_frames[i]:0;
            range=merge_ranges(range,paused_?frozen_.channel_range(i,cutoff):store_.channel_range(i,cutoff));
        }
        return range;
    }
    std::vector<PlotPoint> visible_points(const PlotWindow& panel, std::size_t channel,
                                         double begin, double end, std::size_t width) const {
        const auto range = visible_range(panel);
        if (range.empty) return {};
        begin = std::max(begin, range.begin);
        if (begin > end) return {};
        end = std::min(end, range.end);
        const auto cutoff = std::max(store_.channel_first_frame(channel),channel<panel.first_visible_frames.size()?panel.first_visible_frames[channel]:0);
        return paused_ ? frozen_.query(channel, begin, end, width, cutoff) : store_.query(channel, begin, end, width, cutoff);
    }
    double live_position(const PlotWindow& panel) const {
        if(!link_x_) return store_.position(panel.selected);
        // Linking shares the timeline of signals actually selected in open
        // windows, not every producer that happens to be connected.
        std::vector<bool> selected(store_.channels().size(),false);
        for(const auto& window:panels_) if(window.open)
            for(std::size_t i=0;i<window.selected.size();++i) selected[i]=selected[i] || window.selected[i];
        return store_.position(selected);
    }
    void align_to_latest(PlotWindow& panel, DataRange range) {
        const double right = range.empty ? panel.follow_span :
            paused_ ? range.end : std::max(range.begin,live_position(panel));
        panel.limits.x = {right - panel.follow_span, right};
    }
    void signal_selector(PlotWindow& panel) {
        const auto& channels = store_.channels();
        for (std::size_t i = 0; i < channels.size(); ++i) {
            if (!source_connected_ || !store_.online(i)) continue;
            ImGui::PushID(static_cast<int>(i));
            ImGui::PushStyleColor(ImGuiCol_Text, signal_colors[i % signal_colors.size()]);
            bool selected = panel.selected[i];
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Checkbox(store_.label(i).c_str(), &selected)) panel.selected[i] = selected;
            if (ImGui::IsItemHovered() && !channels[i].id.empty()) {
                ImGui::BeginTooltip();
                if (!channels[i].unit.empty()) ImGui::Text("单位: %s", channels[i].unit.c_str());
                ImGui::TextUnformatted(source_connected_ && store_.online(i) ? "正在更新" : "已停止更新，保留历史数据");
                ImGui::EndTooltip();
            }
            ImGui::PopID();
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
    }
    static bool toggle_menu_item(const char* label, bool& value) {
        const float scale = ImGui::GetStyle().FontScaleDpi;
        const float box = 12 * scale;
        const float width = std::max(ImGui::GetContentRegionAvail().x,
            ImGui::CalcTextSize(label).x + 36 * scale);
        const bool pressed = ImGui::Selectable(label, false, ImGuiSelectableFlags_NoAutoClosePopups,
            {width, ImGui::GetFrameHeight()});
        if (pressed) value = !value;
        const auto lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
        const ImVec2 pos{hi.x - box - 4 * scale, (lo.y + hi.y - box) * 0.5f};
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddRect(pos, {pos.x + box, pos.y + box}, ImGui::GetColorU32(ImGuiCol_TextDisabled), 2 * scale);
        if (value) {
            const ImVec2 mark[] = {{pos.x + 2 * scale, pos.y + 6 * scale},
                {pos.x + 5 * scale, pos.y + 9 * scale}, {pos.x + 10 * scale, pos.y + 3 * scale}};
            draw->AddPolyline(mark, 3, ImGui::GetColorU32(ImGuiCol_Text), ImDrawFlags_None, 1.5f * scale);
        }
        return pressed;
    }
    void draw_menu() {
        if (!ImGui::BeginMainMenuBar()) return;
        if (ImGui::MenuItem("新增窗口", nullptr, false, panels_.size() < 6)) add_window({});
        if (ImGui::BeginMenu("设置")) {
            if (ImGui::BeginMenu("缓冲区点数")) {
                if (ImGui::IsWindowAppearing()) {
                    buffer_points_ = static_cast<int>(store_.capacity_frames());
                    buffer_error_.clear();
                }
                ImGui::SetNextItemWidth(140 * ImGui::GetStyle().FontScaleDpi);
                ImGui::InputInt("##buffer_points", &buffer_points_, 0, 0);
                const bool valid = buffer_points_ >= 8 && buffer_points_ <= 2000000;
                if (!valid && ImGui::IsItemHovered()) ImGui::SetTooltip("请输入 8 到 2,000,000 之间的点数");
                ImGui::SameLine();
                ImGui::BeginDisabled(!valid);
                if (ImGui::Button("应用")) {
                    try {
                        store_.set_capacity_frames(static_cast<std::size_t>(buffer_points_));
                        buffer_error_.clear();
                        ImGui::CloseCurrentPopup();
                    } catch (const std::exception&) {
                        buffer_error_ = "调整失败，原缓存已保留。请尝试更小的点数。";
                    }
                }
                ImGui::EndDisabled();
                if (!buffer_error_.empty()) ImGui::TextUnformatted(buffer_error_.c_str());
                ImGui::EndMenu();
            }
            toggle_menu_item("X 轴联动", link_x_);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("同步所有窗口的 X 轴；实时保持跟随，Y 轴独立");
            if (ImGui::MenuItem("Y 轴自适应", nullptr, false, !panels_.empty())) {
                for (auto& panel : panels_) panel.auto_y = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("清空全部缓冲区")) clear_buffer();
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
    DragZoomLimits zoom_limits() const {
        DragZoomLimits result;
        result.sample_rate = navigation_rate();
        result.max_x_span = std::max(3600.0, store_.reception_time() ? (store_.range().end-store_.range().begin)*2 : static_cast<double>(store_.capacity_frames()) / store_.sample_rate() * 2);
        result.pixels_per_e_fold = default_zoom_pixels_per_e_fold * ImGui::GetStyle().FontScaleDpi;
        return result;
    }
    void auto_view(PlotWindow& panel) {
        if (!paused_) return;
        DataRange available;
        auto included=[&](std::size_t i) {return panel.selected[i] &&
            (i>=panel.legend_hidden.size() || !panel.legend_hidden[i]);};
        for(std::size_t i=0;i<panel.selected.size();++i) if(included(i)) {
            const auto cutoff=std::max(store_.channel_first_frame(i),
                i<panel.first_visible_frames.size()?panel.first_visible_frames[i]:0);
            const auto r=frozen_.channel_range(i,cutoff);
            if(r.empty) continue;
            available=merge_ranges(available,r);
        }
        if(available.empty) return;
        AxisRange next{available.begin,available.end};
        if(next.max<=next.min) {
            const double half=std::max(1e-6,panel.follow_span*0.5);
            next={available.begin-half,available.end+half};
        }
        double low=0,high=0; bool have=false;
        for(std::size_t i=0;i<panel.selected.size();++i) if(included(i)) {
            for(const auto& p:visible_points(panel,i,next.min,next.max,
                    static_cast<std::size_t>(std::max(1.0f,panel.plot_size.x)))) {
                if(!have) {low=high=p.value; have=true;}
                low=std::min(low,p.value); high=std::max(high,p.value);
            }
        }
        panel.limits.x=next; panel.follow_span=next.max-next.min;
        if(have) {
            const double margin=std::max(high-low,std::max(1.0,std::abs(high))*0.02)*0.1;
            panel.limits.y={low-margin,high+margin};
        }
    }
    void navigate(PlotWindow& panel) {
        auto& io = ImGui::GetIO();
        if (!panel.initialized || io.AppFocusLost || any_popup()) {
            panel.box = {};
            panel.zoom = {};
            panel.pan = {}; panel.box = {};
            panel.overview_drag = {};
            return;
        }
        if (panel.overview_drag.held) return;
        const bool window_hovered = ImGui::IsWindowHovered() && !over_overview(panel, io.MousePos);
        const bool hovered = window_hovered && contains(io.MousePos, panel.plot_pos, panel.plot_size);
        const bool in_frame = window_hovered && contains(io.MousePos, panel.frame_pos, panel.frame_size);
        const bool x_labels = in_frame && io.MousePos.x >= panel.plot_pos.x &&
            io.MousePos.x < panel.plot_pos.x + panel.plot_size.x &&
            io.MousePos.y >= panel.plot_pos.y + panel.plot_size.y;
        const bool y_labels = in_frame && io.MousePos.x < panel.plot_pos.x &&
            io.MousePos.y >= panel.plot_pos.y && io.MousePos.y < panel.plot_pos.y + panel.plot_size.y;
        const bool over_legend = contains(io.MousePos, panel.legend_pos, panel.legend_size);
        if (paused_ && hovered && !over_legend && !panel.pan.held && !panel.zoom.held &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Middle)) {
            active_id_=panel.id;
            panel.box={};
            auto_view(panel);
            return;
        }
        if (!paused_ || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            if (panel.box.held) {panel.box = {}; return;}
        }
        if (paused_ && hovered && !over_legend && ImGui::IsMouseClicked(ImGuiMouseButton_Middle) &&
            !panel.pan.held && !panel.zoom.held) {
            active_id_ = panel.id;
            panel.box = {true, io.MousePos, io.MousePos, panel.plot_pos, panel.plot_size, panel.limits};
        }
        if (panel.box.held) {
            auto& box = panel.box;
            if (box.size.x != panel.plot_size.x || box.size.y != panel.plot_size.y) {box = {}; return;}
            box.end = {std::clamp(io.MousePos.x, box.origin.x, box.origin.x + box.size.x),
                       std::clamp(io.MousePos.y, box.origin.y, box.origin.y + box.size.y)};
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Middle)) {
                const float threshold = 5 * ImGui::GetStyle().FontScaleDpi;
                if (std::abs(box.end.x-box.start.x) >= threshold && std::abs(box.end.y-box.start.y) >= threshold) {
                    const auto x = [&](float pixel) {return box.initial.x.min + (pixel-box.origin.x)/box.size.x * (box.initial.x.max-box.initial.x.min);};
                    const auto y = [&](float pixel) {return box.initial.y.max - (pixel-box.origin.y)/box.size.y * (box.initial.y.max-box.initial.y.min);};
                    panel.limits.x = {x(std::min(box.start.x,box.end.x)), x(std::max(box.start.x,box.end.x))};
                    panel.limits.y = {y(std::max(box.start.y,box.end.y)), y(std::min(box.start.y,box.end.y))};
                    panel.follow_span = panel.limits.x.max-panel.limits.x.min;
                    panel.auto_y = false;
                }
                box = {};
            } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) box = {};
            return;
        }
        if ((hovered && (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1))) ||
            ((hovered || x_labels || y_labels) && io.MouseWheel != 0)) active_id_ = panel.id;
        if (hovered && !over_legend && ImGui::IsMouseDoubleClicked(0)) {
            toggle_pause(store_.range());
            return;
        }
        if ((hovered || x_labels || y_labels) && ImGui::IsMouseClicked(1) && !panel.pan.held) {
            active_id_ = panel.id;
            panel.zoom = {true, false, io.MousePos, panel.limits, anchor_at(panel, io.MousePos),
                x_labels ? ZoomAxes::XOnly : y_labels ? ZoomAxes::YOnly :
                io.KeyShift ? ZoomAxes::XOnly : io.KeyCtrl ? ZoomAxes::YOnly : ZoomAxes::Both};
            if (panel.follow && !paused_) panel.zoom.anchor.x = panel.limits.x.max;
        }
        if (panel.zoom.held) {
            auto& gesture = panel.zoom;
            const double dx = io.MousePos.x - gesture.press.x, dy = io.MousePos.y - gesture.press.y;
            const double threshold = 3 * ImGui::GetStyle().FontScaleDpi;
            gesture.dragged |= dx * dx + dy * dy >= threshold * threshold;
            if (gesture.dragged) {
                if (gesture.axes != ZoomAxes::XOnly && dy != 0) panel.auto_y = false;
                constexpr double right_drag_gain = 1.5;
                auto next = zoom_from_drag(gesture.initial, gesture.anchor,
                    dx * right_drag_gain, dy * right_drag_gain, gesture.axes, zoom_limits());
                // Do not restore the press-time Y range every held frame:
                // auto-fit has already updated it for the current visible data.
                if (gesture.axes == ZoomAxes::XOnly || panel.auto_y) next.y = panel.limits.y;
                if (gesture.axes == ZoomAxes::YOnly) next.x = panel.limits.x;
                panel.limits = next;
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
        if ((x_labels || y_labels || (hovered && !over_legend)) && ImGui::IsMouseClicked(0)) {
            active_id_ = panel.id;
            panel.pan = {true, false, io.MousePos, panel.limits, {},
                x_labels ? ZoomAxes::XOnly : y_labels ? ZoomAxes::YOnly : ZoomAxes::Both};
        }
        if (panel.pan.held) {
            auto& gesture = panel.pan;
            const double dx = gesture.axes == ZoomAxes::YOnly ? 0 : io.MousePos.x - gesture.press.x;
            const double dy = gesture.axes == ZoomAxes::XOnly ? 0 : io.MousePos.y - gesture.press.y;
            const double threshold = 4 * ImGui::GetStyle().FontScaleDpi;
            gesture.dragged |= dx * dx + dy * dy >= threshold * threshold;
            if (gesture.dragged) {
                const auto& initial = gesture.initial;
                if (gesture.axes != ZoomAxes::XOnly) {
                    if (dy != 0) panel.auto_y = false;
                    const double ty = dy / std::max(1.0f, panel.plot_size.y) * (initial.y.max - initial.y.min);
                    panel.limits.y = {initial.y.min + ty, initial.y.max + ty};
                }
                if (gesture.axes != ZoomAxes::YOnly && (!panel.follow || paused_)) {
                    const double tx = -dx / std::max(1.0f, panel.plot_size.x) * (initial.x.max - initial.x.min);
                    panel.limits.x = {initial.x.min + tx, initial.x.max + tx};
                }
            }
            if (ImGui::IsMouseReleased(0) || !ImGui::IsMouseDown(0)) gesture = {};
            return;
        }
        if ((hovered || x_labels || y_labels) && !over_legend && io.MouseWheel != 0) {
            const double delta = std::clamp(double(io.MouseWheel), -8.0, 8.0) * 35 * ImGui::GetStyle().FontScaleDpi;
            const auto axes = x_labels ? ZoomAxes::XOnly : y_labels || io.KeyCtrl ? ZoomAxes::YOnly : ZoomAxes::Both;
            auto anchor = anchor_at(panel, io.MousePos);
            if (axes != ZoomAxes::YOnly && panel.follow && !paused_) anchor.x = panel.limits.x.max;
            panel.limits = zoom_from_drag(panel.limits, anchor, delta, -delta,
                axes, zoom_limits());
            if (axes != ZoomAxes::XOnly) panel.auto_y = false;
            if (axes != ZoomAxes::YOnly) panel.follow_span = panel.limits.x.max - panel.limits.x.min;
        }
    }
    void draw_overview(PlotWindow& panel, DataRange range) {
        // The live overview must use the same playhead as the main plot.
        // Drawing the received-but-not-yet-presented tail makes the viewport's
        // right edge oscillate as each batch arrives and the playhead catches up.
        // Once the ring is full, translate both bounds together. Using the raw
        // eviction boundary with a smooth end changes the overview's scale on
        // every packet, making even a fixed-width viewport breathe horizontally.
        // Queries still clip to actual retained samples; an evicted left margin
        // stays empty instead of inventing values.
        if (!paused_ && !range.empty) {
            const auto end = std::clamp(live_position(panel), range.begin, range.end);
            if (range.frame_count >= store_.capacity_frames()) range.begin -= range.end - end;
            range.end = end;
        }
        const float scale = ImGui::GetStyle().FontScaleDpi;
        const auto saved_cursor = ImGui::GetCursorScreenPos();
        panel.overview_size = {std::min(320 * scale, panel.plot_size.x - 12 * scale),
            std::min(100 * scale, panel.plot_size.y - 32 * scale)};
        panel.overview_pos = {panel.plot_pos.x + panel.plot_size.x - panel.overview_size.x - 6 * scale,
            panel.plot_pos.y + 26 * scale};
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
                    const double minimum = std::min(8 / navigation_rate(), duration);
                    const bool following = panel.follow && !paused_;
                    auto to_pixel = [&](double t) {
                        return graph_pos.x + static_cast<float>(std::clamp((t - extent.min) / duration, 0.0, 1.0)) * graph_size.x;
                    };
                    auto selection_pixels = [&]() {
                        const float left = to_pixel(panel.limits.x.min), right = to_pixel(panel.limits.x.max);
                        return ImVec2{left, right};
                    };
                    auto apply_range = [&](AxisRange value) {
                        panel.limits.x = value;
                        panel.follow_span = value.max - value.min;
                        if (following) align_to_latest(panel, range);
                    };
                    auto edges = selection_pixels();
                    const float grip = 5 * scale;
                    const float center = (edges.x + edges.y) * 0.5f;
                    const bool narrow = edges.y - edges.x < 2 * grip + 4 * scale;
                    // A narrow range keeps a small central move target. Invisible
                    // resize targets sit to either side, without widening the drawing.
                    const bool in_box = narrow ? std::abs(io.MousePos.x - center) <= 2 * scale
                        : io.MousePos.x >= edges.x && io.MousePos.x <= edges.y;
                    const bool at_left = narrow ? io.MousePos.x < center - 2 * scale && io.MousePos.x >= edges.x - grip
                        : std::abs(io.MousePos.x - edges.x) <= grip;
                    const bool at_right = narrow ? io.MousePos.x > center + 2 * scale && io.MousePos.x <= edges.y + grip
                        : std::abs(io.MousePos.x - edges.y) <= grip;
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
                    const auto width = static_cast<std::size_t>(std::max(1.0f, graph_size.x));
                    std::vector<std::vector<PlotPoint>> series(panel.selected.size());
                    double low = 0, high = 0;
                    bool have_points = false;
                    for (std::size_t channel = 0; channel < panel.selected.size(); ++channel) {
                        if (!panel.selected[channel]) continue;
                        auto& points = series[channel];
                        points = visible_points(panel, channel, extent.min, extent.max, width);
                        for (const auto& point : points) {
                            if (!have_points) { low = high = point.value; have_points = true; }
                            low = std::min(low, point.value);
                            high = std::max(high, point.value);
                        }
                    }
                    // All selected signals share one scale, preserving relative amplitudes.
                    const double middle = (low + high) * 0.5;
                    const double y_span = std::max(1e-9, high - low) * 1.2;
                    for (std::size_t channel = 0; channel < series.size(); ++channel) {
                        const auto& points = series[channel];
                        if (points.empty()) continue;
                        std::vector<ImVec2> preview;
                        preview.reserve(points.size());
                        for (const auto& point : points) preview.push_back({to_pixel(point.t),
                            graph_pos.y + graph_size.y * static_cast<float>(0.5 - (point.value - middle) / y_span)});
                        auto color = signal_colors[channel % signal_colors.size()];
                        color.w = 0.55f;
                        if (points.size() > static_cast<std::size_t>(graph_size.x * 2)) {
                            // Dense previews show a per-pixel min/max envelope instead
                            // of connecting aliased oscillations across the whole cache.
                            int column = -1;
                            float top = 0, bottom = 0;
                            auto flush = [&]() {
                                if (column < 0) return;
                                const float x = graph_pos.x + static_cast<float>(column) + 0.5f;
                                draw->AddLine({x, top}, {x, std::max(top + scale, bottom)}, ImGui::GetColorU32(color), scale);
                            };
                            for (const auto& point : preview) {
                                const int next = std::clamp(static_cast<int>(point.x - graph_pos.x), 0,
                                    std::max(0, static_cast<int>(graph_size.x) - 1));
                                if (next != column) { flush(); column = next; top = bottom = point.y; }
                                else { top = std::min(top, point.y); bottom = std::max(bottom, point.y); }
                            }
                            flush();
                        } else {
                            draw->AddPolyline(preview.data(), static_cast<int>(preview.size()), ImGui::GetColorU32(color), ImDrawFlags_None, scale);
                        }
                        points_this_frame_ += points.size();
                    }
                    edges = selection_pixels();
                    // Dim only the context; keep the actual viewport a fine outline.
                    draw->AddRectFilled(graph_pos, {edges.x, graph_end.y}, IM_COL32(0, 0, 0, 95));
                    draw->AddRectFilled({edges.y, graph_pos.y}, graph_end, IM_COL32(0, 0, 0, 95));
                    if (edges.y - edges.x >= scale) {
                        draw->AddRect({edges.x, graph_pos.y + scale}, {edges.y, graph_end.y - scale},
                            IM_COL32(151, 184, 206, 220), 0, ImDrawFlags_None, scale);
                    } else {
                        // Below one physical pixel, use a hairline at the true center.
                        const float x = (edges.x + edges.y) * 0.5f;
                        draw->AddLine({x, graph_pos.y + scale}, {x, graph_end.y - scale},
                            IM_COL32(151, 184, 206, 220), scale);
                    }
                    if (hovered && input_allowed && !panel.overview_drag.held) {
                        const double span = panel.limits.x.max - panel.limits.x.min;
                        if (span < 0.001) ImGui::SetTooltip("%.1f us", span * 1e6);
                        else if (span < 1) ImGui::SetTooltip("%.2f ms", span * 1e3);
                        else ImGui::SetTooltip("%.3f s", span);
                    }
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
                const auto right_drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right, 0);
                if (ImGui::IsWindowHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
                    right_drag.x * right_drag.x + right_drag.y * right_drag.y < 9 * scale * scale)
                    ImGui::OpenPopup("overview_context");
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8 * scale, 8 * scale});
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8 * scale, 5 * scale});
                if (ImGui::BeginPopup("overview_context")) {
                    if (toggle_menu_item("Zoom", panel.overview_open)) {
                        panel.overview_drag = {};
                        if (!panel.overview_open) ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopStyleVar(2);
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
        const auto range = visible_range(panel);
        const bool has_signals = std::any_of(panel.selected.begin(), panel.selected.end(), [](bool b) { return b; });
        if (has_signals && (!panel.x_range_initialized || (panel.follow && !paused_ && !range.empty)))
            align_to_latest(panel, range);
        panel.x_range_initialized = true;
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
            const double samples = (limits.X.Max - limits.X.Min) * navigation_rate();
            const float marker_size = samples < width / 3.0 ? 1.28f : samples < width ? 0.8f : 0.38f;
            bool any_points = false;
            panel.legend_hidden.resize(panel.selected.size(),false);
            for (std::size_t i = 0; i < panel.selected.size(); ++i) {
                if (!panel.selected[i]) continue;
                const auto points = visible_points(panel, i, limits.X.Min, limits.X.Max, width);
                if (points.empty()) continue;
                ImPlotSpec spec;
                const auto color = signal_colors[i % signal_colors.size()];
                spec.LineColor = color;
                spec.LineWeight = 1.2f * ImGui::GetStyle().FontScaleDpi;
                spec.Marker = panel.markers || points.size()==1 ? ImPlotMarker_Circle : ImPlotMarker_None;
                spec.MarkerSize = (store_.reception_time(i) ? 1.8f : marker_size) * ImGui::GetStyle().FontScaleDpi;
                spec.MarkerFillColor = color;
                spec.MarkerLineColor = {0, 0, 0, 0};
                spec.Stride = sizeof(PlotPoint);
                const std::string label = store_.label(i)
                    + "###channel" + std::to_string(i);
                ImPlot::PlotLine(label.c_str(), &points[0].t, &points[0].value, static_cast<int>(points.size()), spec);
                if (const auto* item=ImPlot::GetItem(label.c_str())) panel.legend_hidden[i]=!item->Show;
                any_points = true;
                points_this_frame_ += points.size();
            }
            const auto pos = panel.plot_pos, size = panel.plot_size;
            auto* draw_list = ImPlot::GetPlotDrawList();
            draw_list->AddLine(pos, {pos.x, pos.y + size.y}, IM_COL32(135, 135, 135, 255));
            draw_list->AddLine({pos.x, pos.y + size.y}, {pos.x + size.x, pos.y + size.y}, IM_COL32(135, 135, 135, 255));
            if (!has_signals || !any_points) {
                const char* hint = !has_signals ? "右键添加信号" : range.empty ? "暂无数据" : paused_
                    ? "此区间不在冻结缓存内" : "此区间已超出缓存";
                const auto text_size = ImGui::CalcTextSize(hint);
                draw_list->AddText({pos.x + (size.x - text_size.x) / 2, pos.y + size.y / 2},
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), hint);
            }
            auto* plot = ImPlot::GetCurrentPlot();
            panel.frame_pos = plot->FrameRect.Min;
            panel.frame_size = plot->FrameRect.GetSize();
            const int legend_vertex_start = draw_list->VtxBuffer.Size;
            ImPlot::EndPlot();
            // ImPlot dims hidden legend entries to 25% alpha. Keep those
            // labels readable on black without changing the vendored library.
            const ImU32 hidden_color = ImAlphaU32(ImPlot::GetStyleColorU32(ImPlotCol_LegendText), 0.25f);
            const ImU32 readable_hidden = ImGui::GetColorU32(ImVec4{0.58f, 0.58f, 0.58f, 1});
            for (int i = legend_vertex_start; i < draw_list->VtxBuffer.Size; ++i) {
                auto& vertex = draw_list->VtxBuffer[i];
                if (vertex.col == hidden_color && plot->Items.Legend.Rect.Contains(vertex.pos))
                    vertex.col = readable_hidden;
            }
            panel.legend_pos = plot->Items.Legend.RectClamped.Min;
            panel.legend_size = plot->Items.Legend.RectClamped.GetSize();
            if (panel.box.held) {
                const auto& box = panel.box;
                draw_list->PushClipRect(panel.plot_pos, {panel.plot_pos.x+panel.plot_size.x,panel.plot_pos.y+panel.plot_size.y}, true);
                draw_list->AddRect({std::min(box.start.x,box.end.x),std::min(box.start.y,box.end.y)},
                                   {std::max(box.start.x,box.end.x),std::max(box.start.y,box.end.y)},
                                   IM_COL32(240,240,240,255), 0, ImDrawFlags_None, ImGui::GetStyle().FontScaleDpi);
                draw_list->PopClipRect();
            }
            draw_overview(panel, range);
        }
        // The canvas uses zero padding/spacing; do not inherit that in menus.
        const float menu_scale = ImGui::GetStyle().FontScaleDpi;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8 * menu_scale, 8 * menu_scale});
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8 * menu_scale, 5 * menu_scale});
        if (ImGui::BeginPopup("plot_context")) {
            signal_selector(panel);
            ImGui::Separator();
            toggle_menu_item("Y 轴自适应", panel.auto_y);
            const float scale = ImGui::GetStyle().FontScaleDpi;
            const float box = 12 * scale;
            const float row_width = std::max(ImGui::GetContentRegionAvail().x, ImGui::CalcTextSize("清空数据").x + 16 * scale);
            if (ImGui::Selectable("Zoom", false, ImGuiSelectableFlags_NoAutoClosePopups,
                {row_width, ImGui::GetFrameHeight()})) {
                panel.overview_open = !panel.overview_open;
                panel.overview_drag = {};
            }
            const auto row_min = ImGui::GetItemRectMin(), row_max = ImGui::GetItemRectMax();
            const ImVec2 check_pos{row_max.x - box - 4 * scale, (row_min.y + row_max.y - box) * 0.5f};
            auto* menu_draw = ImGui::GetWindowDrawList();
            menu_draw->AddRect(check_pos, {check_pos.x + box, check_pos.y + box},
                ImGui::GetColorU32(ImGuiCol_TextDisabled), 2 * scale);
            if (panel.overview_open) {
                const ImVec2 mark[] = {{check_pos.x + 2 * scale, check_pos.y + 6 * scale},
                    {check_pos.x + 5 * scale, check_pos.y + 9 * scale},
                    {check_pos.x + 10 * scale, check_pos.y + 3 * scale}};
                menu_draw->AddPolyline(mark, 3, ImGui::GetColorU32(ImGuiCol_Text), ImDrawFlags_None, 1.5f * scale);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("单击开启 / 关闭当前窗口的 Zoom 缩略图");
            ImGui::Separator();
            if (ImGui::Selectable("清空数据", false, ImGuiSelectableFlags_None,
                {row_width, ImGui::GetFrameHeight()})) clear_window_data(panel);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("仅本窗口从此刻重新显示数据，其他窗口不受影响");
            if (ImGui::Selectable("删除窗口", false, ImGuiSelectableFlags_None,
                {row_width, ImGui::GetFrameHeight()})) panel.open = false;
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(2);
    }
    void draw_performance(DataRange range) {
        ImGui::SetNextWindowSize({280 * ImGui::GetStyle().FontScaleDpi, 0}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("性能信息", &show_performance_, ImGuiWindowFlags_AlwaysAutoResize)) {
            if (store_.reception_time()) ImGui::Text("接收时间 · %zu 通道", store_.channels().size());
            else ImGui::Text("%.0f kHz / 通道 · %zu 通道", store_.sample_rate() / 1000, store_.channels().size());
            const auto displayed_range = paused_ ? frozen_.range() : range;
            ImGui::Text("显示缓存 %zu / %zu 点/通道", displayed_range.frame_count, store_.capacity_frames());
            if (paused_) ImGui::TextDisabled("已冻结；后台接收继续");
            ImGui::Text("%.0f FPS · %zu 绘制点", ImGui::GetIO().Framerate, points_this_frame_);
        }
        ImGui::End();
    }
};

WaveView::WaveView(DataStore& store, double initial_span) : impl_(std::make_unique<Impl>(store, initial_span)) {}
WaveView::WaveView(SignalHub& store, double initial_span) : impl_(std::make_unique<Impl>(store, initial_span)) {}
WaveView::~WaveView() = default;
void WaveView::draw() { impl_->draw(); }
void WaveView::set_source_connected(bool connected) { impl_->source_connected_ = connected; }
std::size_t WaveView::points_this_frame() const { return impl_->points_this_frame(); }
} // namespace wave
