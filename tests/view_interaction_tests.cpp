#include "wave_view.hpp"
#include "signal_hub.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "implot.h"
#include "implot_internal.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// Headless integration tests: the real view consumes ImGui's queued mouse
// events and runs the real ImPlot frame lifecycle. No platform or renderer
// backend, native window, production test hook, or persistent UI state is used.
namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool near(double a, double b, double tolerance = 2e-6) {
    return std::abs(a - b) <= tolerance * std::max({1.0, std::abs(a), std::abs(b)});
}

struct PlotState {
    ImRect rect;
    ImPlotRange x;
    ImPlotRange y;
};

class Fixture {
public:
    Fixture(std::vector<wave::ChannelInfo> channels = {{"Sine", "V"}}, wave::SignalHub* hub=nullptr) : store(20'000.0, 5.0, channels) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImPlot::CreateContext();
        auto& io = ImGui::GetIO();
        io.DisplaySize = {1200, 700};
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigInputTrickleEventQueue = false;
        // Match the DX11 backend: multiple preview curves may exceed 64K vertices.
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
        io.Fonts->AddFontDefault();
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        require(pixels && width > 0 && height > 0, "Font atlas must be ready before NewFrame");
        io.Fonts->SetTexID(static_cast<ImTextureID>(1));
        wave::apply_style();
        append_second(0, 1.0f);
        view = hub ? std::make_unique<wave::WaveView>(*hub) : std::make_unique<wave::WaveView>(store);
        for (int i = 0; i < 8; ++i) frame();
        require(GImPlot->Plots.GetAliveCount() > 0, "WaveView must create an ImPlot plot");
        for (int i = 0; i < GImPlot->Plots.GetMapSize(); ++i) {
            if (auto* candidate = GImPlot->Plots.TryGetMapData(i)) {
                plot_id = candidate->ID;
                break;
            }
        }
        const auto initial = state();
        require(initial.rect.GetWidth() > 100 && initial.rect.GetHeight() > 100,
                "Wave plot must have a usable visible rectangle");
        require(initial.x.Size() > 0 && initial.y.Size() > 0, "Initial axes must have finite positive ranges");
        require(!popup_open(), "Test must start without an open popup");
        require(points_drawn() == 0, "Initial window must not automatically select a channel");
        append_second(1, 1.0f);
        frame();
        require(points_drawn() == 0, "Incoming data must not automatically select a channel");
        require(near(state().x.Min, initial.x.Min) && near(state().x.Max, initial.x.Max),
                "Incoming data must not scroll an empty window's X axis");
        toggle_channel(0);
        for (int i = 0; i < 8; ++i) frame();
    }

    wave::DataStore& test_store() { return store; }
    ~Fixture() {
        view.reset();
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
    }

    void frame() {
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        view->draw();
        ImGui::Render();
    }

    void move(ImVec2 point) {
        ImGui::GetIO().AddMousePosEvent(point.x, point.y);
        frame();
    }

    void right(bool down) {
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, down);
        frame();
    }

    void left(bool down) {
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, down);
        frame();
    }

    void wheel(float amount) {
        ImGui::GetIO().AddMouseWheelEvent(0.0f, amount);
        frame();
    }

    void press_space() {
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Space, true);
        frame();
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Space, false);
        frame();
    }

    void press_key(ImGuiKey key) {
        ImGui::GetIO().AddKeyEvent(key, true);
        frame();
        ImGui::GetIO().AddKeyEvent(key, false);
        frame();
    }

    void double_click(ImVec2 point) {
        move(point);
        // Separate this pair from any previous left click without sleeping:
        // each headless frame advances ImGui's clock by 1/60 second.
        const int settle_frames = static_cast<int>(std::ceil(ImGui::GetIO().MouseDoubleClickTime * 60.0f)) + 2;
        for (int i = 0; i < settle_frames; ++i) frame();
        left(true);
        left(false);
        left(true);
        require(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left),
                "Queued clicks must be recognized as a real ImGui double click");
        left(false);
        frame();
    }

    PlotState state() const {
        const auto* plot = GImPlot->Plots.GetByKey(plot_id);
        require(plot != nullptr, "Wave plot ID must survive input frames");
        return {plot->PlotRect, plot->Axes[ImAxis_X1].Range, plot->Axes[ImAxis_Y1].Range};
    }

    bool popup_open() const { return GImGui->OpenPopupStack.Size != 0; }

    ImVec2 legend_point() const {
        const auto* plot = GImPlot->Plots.GetByKey(plot_id);
        require(plot && plot->Items.GetLegendCount() == 1, "Fixture must show one signal legend entry");
        const auto rect = plot->Items.Legend.RectClamped;
        require(rect.GetWidth() > 0 && rect.GetHeight() > 0, "Legend must have a visible hit rectangle");
        const auto point = rect.GetCenter();
        return {std::floor(point.x), std::floor(point.y)};
    }

    bool signal_shown() const {
        auto* plot = GImPlot->Plots.GetByKey(plot_id);
        require(plot && plot->Items.GetLegendCount() == 1, "Fixture must retain one legend entry");
        return plot->Items.GetLegendItem(0)->Show;
    }

    ImVec2 press_point() const {
        const auto rect = state().rect;
        // Deliberately off-centre: a centre-based zoom must fail the anchor test.
        // ImGui::UpdateMouseInputs floors queued positions to screen pixels.
        // Inject integer pixels so expected anchors use the consumed position.
        return {std::floor(rect.Min.x + rect.GetWidth() * 0.37f),
                std::floor(rect.Min.y + rect.GetHeight() * 0.62f)};
    }

    ImVec2 x_axis_point() const {
        const auto* plot = GImPlot->Plots.GetByKey(plot_id);
        return {std::floor(plot->PlotRect.GetCenter().x),
            std::floor((plot->PlotRect.Max.y + plot->FrameRect.Max.y) * 0.5f)};
    }
    void middle(bool down) {
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Middle, down);
        frame();
    }
    void middle_double_click(ImVec2 point) {
        move(point);
        for(int i=0;i<32;++i) frame();
        middle(true); middle(false); middle(true);
        require(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Middle), "Middle double click must be recognized");
        middle(false); frame();
    }
    ImVec2 y_axis_point(bool inside_plot) const {
        const auto* plot = GImPlot->Plots.GetByKey(plot_id);
        require(plot != nullptr, "Y-axis hit test needs a live plot");
        const auto rect = plot->PlotRect;
        const float x = inside_plot ? rect.Min.x + 20.0f :
            (plot->FrameRect.Min.x + rect.Min.x) * 0.5f;
        const ImVec2 point{std::floor(x),
                           std::floor(rect.Min.y + rect.GetHeight() * 0.62f)};
        require(plot->FrameRect.Contains(point), "Y-axis wheel target must remain inside the plot frame");
        require(inside_plot ? rect.Contains(point) : point.x < rect.Min.x,
                "Y-axis wheel test must distinguish the label margin from the plot interior");
        return point;
    }

    double latest_time() const { return store.range().end; }

    std::size_t points_drawn() const { return view->points_this_frame(); }
    void clear_channel_from_source(std::size_t channel) { store.clear_channel(channel); frame(); }
    void register_external_channels() {
        store.configure(10, {{"Current", "A", "ia"}, {"Temperature", "C", "temp"}});
        store.append(0, std::vector<float>{1,20,2,21,3,22});
        frame();
    }

    void click(ImVec2 point) {
        move(point);
        left(true);
        left(false);
        frame();
    }

    void toggle_channel(std::size_t channel) {
        move(press_point());
        right(true);
        right(false);
        frame();
        require(popup_open(), "Channel selector must open");
        const auto* popup = GImGui->OpenPopupStack.back().Window;
        require(popup != nullptr, "Channel selector must have a window");
        const auto start = popup->DC.CursorStartPos;
        const float row = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;
        click({start.x + 8, start.y + static_cast<float>(channel) * row + ImGui::GetFrameHeight() * 0.5f});
        press_key(ImGuiKey_Escape);
        frame();
    }

    bool overview_has_color(ImVec4 color) const {
        const auto* window = overview_window();
        require(window != nullptr, "Overview must be open for rendering checks");
        const auto expected = ImGui::GetColorU32(color);
        for (const auto& vertex : window->DrawList->VtxBuffer)
            if (vertex.col == expected) return true;
        return false;
    }

    void require_unitless_legend() const {
        auto* plot = GImPlot->Plots.GetByKey(plot_id);
        require(plot != nullptr, "Main plot must exist");
        for (int i = 0; i < plot->Items.GetLegendCount(); ++i)
            require(std::string(plot->Items.GetLegendLabel(i)).find('[') == std::string::npos,
                    "Legend must omit unit suffixes even when channel metadata has a unit");
    }
    float overview_outline_width() const {
        const auto* window = overview_window();
        require(window != nullptr, "Overview must exist");
        float left = 1e9f, right = -1e9f;
        for (const auto& vertex : window->DrawList->VtxBuffer) {
            if (vertex.col != IM_COL32(151, 184, 206, 220)) continue;
            left = std::min(left, vertex.pos.x);
            right = std::max(right, vertex.pos.x);
        }
        require(right >= left, "Selection outline must be visible");
        return right - left;
    }
    float overview_outline_right() const {
        const auto* window = overview_window();
        require(window != nullptr, "Overview must exist");
        float right = -1e9f;
        for (const auto& vertex : window->DrawList->VtxBuffer)
            if (vertex.col == IM_COL32(151, 184, 206, 220)) right = std::max(right, vertex.pos.x);
        require(right > 0, "Selection outline must be visible");
        return right;
    }
    double rendered_y_tick_extent() const {
        const auto* plot = GImPlot->Plots.GetByKey(plot_id);
        double extent = 0;
        for (const auto& tick : plot->Axes[ImAxis_Y1].Ticker.Ticks)
            if (tick.ShowLabel) extent = std::max(extent, std::abs(tick.PlotPos));
        return extent;
    }
    bool auto_y_enabled() const {
        return (GImPlot->Plots.GetByKey(plot_id)->Axes[ImAxis_Y1].Flags & ImPlotAxisFlags_AutoFit) != 0;
    }
    void toggle_auto_y() {
        move(press_point()); right(true); right(false); frame();
        require(popup_open(), "Y auto-fit menu must open");
        auto* popup = GImGui->OpenPopupStack.back().Window;
        const auto target = ImHashStr("Y 轴自适应", 0, popup->IDStack.back());
        move({-1000, -1000});
        for (int i = 0; i < 12 && GImGui->NavId != target; ++i) press_key(ImGuiKey_DownArrow);
        require(GImGui->NavId == target, "Y auto-fit entry must be reachable");
        press_key(ImGuiKey_Enter); press_key(ImGuiKey_Escape); frame();
    }
    void clear_buffer_via_menu(bool local = false) {
        if (local) { move(press_point()); right(true); right(false); frame(); }
        else {
            const auto* menu = ImGui::FindWindowByName("##MainMenuBar");
            const float x = menu->Pos.x + ImGui::GetStyle().WindowPadding.x +
                ImGui::CalcTextSize("新增窗口").x + 2 * ImGui::GetStyle().ItemSpacing.x +
                ImGui::CalcTextSize("设置").x * 0.5f;
            click({x, menu->Pos.y + menu->Size.y * 0.5f}); frame();
        }
        require(popup_open(), "Buffer clear menu must open");
        auto* popup = GImGui->OpenPopupStack.back().Window;
        const auto target = ImHashStr(local ? "清空数据" : "清空全部缓冲区", 0, popup->IDStack.back());
        move({-1000, -1000});
        for (int i = 0; i < 16 && GImGui->NavId != target; ++i) press_key(ImGuiKey_DownArrow);
        require(GImGui->NavId == target, "Buffer clear entry must be reachable");
        press_key(ImGuiKey_Enter); frame(); frame();
        require(local ? !store.range().empty : store.range().empty,
                "Only global clear may erase shared history");
    }
    void select_other_signal() {
        move(other_panel_state().rect.GetCenter()); right(true); right(false); frame();
        require(popup_open(), "Other panel menu must open");
        const auto* popup = GImGui->OpenPopupStack.back().Window;
        click({popup->DC.CursorStartPos.x + 8, popup->DC.CursorStartPos.y + ImGui::GetFrameHeight() * 0.5f});
        press_key(ImGuiKey_Escape); frame();
    }
    void toggle_overview() {
        const bool was_open = overview_window() != nullptr;
        move(press_point());
        right(true);
        right(false);
        frame();
        require(popup_open(), "Zoom must be available in the plot context menu");
        auto* popup = GImGui->OpenPopupStack.back().Window;
        const auto target = ImHashStr("Zoom", 0, popup->IDStack.back());
        move({-1000, -1000});
        for (int i = 0; i < 12 && GImGui->NavId != target; ++i) press_key(ImGuiKey_DownArrow);
        require(GImGui->NavId == target, "Zoom menu item must be reachable");
        const auto rect = popup->NavRectRel[0];
        const ImVec2 point{popup->Pos.x + rect.GetCenter().x, popup->Pos.y + rect.GetCenter().y};
        click(point);
        require(popup_open() && (overview_window() != nullptr) != was_open,
                "A single click must toggle Zoom while retaining the visible menu state");
        press_key(ImGuiKey_Escape);
        frame();
    }

    ImGuiWindow* overview_window() const {
        const auto rect = state().rect;
        for (auto* window : GImGui->Windows) {
            const ImVec2 center{window->Pos.x + window->Size.x * 0.5f,
                                window->Pos.y + window->Size.y * 0.5f};
            if (window->Active && std::string(window->Name).find("zoom_overview") != std::string::npos &&
                rect.Contains(center))
                return window;
        }
        return nullptr;
    }

    ImRect overview_canvas() const {
        const auto* window = overview_window();
        require(window != nullptr, "Zoom overview must be open before addressing its canvas");
        // The invisible canvas is the first item in this child. Read its
        // actual content origin/limits; only the reserved footer height comes
        // from the overview's public visual layout.
        const ImVec2 start = window->DC.CursorStartPos;
        const float scale = ImGui::GetStyle().FontScaleDpi;
        const ImVec2 end{window->ContentRegionRect.Max.x,
            std::max(start.y + 8.0f * scale,
                window->ContentRegionRect.Max.y - ImGui::GetTextLineHeight() - 3.0f * scale)};
        require(end.x - start.x > 50.0f && end.y - start.y > 8.0f,
                "Overview must expose a usable input canvas");
        return {start, end};
    }

    ImVec2 overview_fraction_point(float fraction) const {
        const auto rect = overview_canvas();
        return {std::floor(rect.Min.x + rect.GetWidth() * fraction),
                std::floor(rect.GetCenter().y)};
    }

    ImVec2 overview_selection_point(double extent_end, int edge = 0) const {
        const auto canvas = overview_canvas();
        const auto selected = state().x;
        const float left = canvas.Min.x + static_cast<float>(selected.Min / extent_end) * canvas.GetWidth();
        const float right = canvas.Min.x + static_cast<float>(selected.Max / extent_end) * canvas.GetWidth();
        require(right - left > 22.0f, "Overview interaction fixture must keep the true selection wider than its minimum hit box");
        const float x = edge < 0 ? left + 1.0f : edge > 0 ? right - 2.0f : (left + right) * 0.5f;
        return {std::floor(x), std::floor(canvas.GetCenter().y)};
    }

    int active_panel_count() const {
        int count = 0;
        for (auto* window : GImGui->Windows)
            if (window->Active && std::string(window->Name).find("/panel_") != std::string::npos &&
                std::string(window->Name).find("zoom_overview") == std::string::npos) ++count;
        return count;
    }

    void remove_first_window() {
        ImGuiWindow* panel = nullptr;
        for (auto* window : GImGui->Windows)
            if (window->Active && std::string(window->Name).find("/panel_") != std::string::npos &&
                std::string(window->Name).find("zoom_overview") == std::string::npos) { panel = window; break; }
        require(panel != nullptr, "Removal needs an active panel");
        move(panel->InnerRect.GetCenter());
        right(true);
        right(false);
        frame();
        require(popup_open(), "Removal menu must open");
        const auto* popup = GImGui->OpenPopupStack.back().Window;
        const auto target = ImHashStr("删除窗口", 0, popup->IDStack.back());
        move({-1000, -1000});
        for (int i = 0; i < 12 && GImGui->NavId != target; ++i) press_key(ImGuiKey_DownArrow);
        require(GImGui->NavId == target, "Removal entry must be reachable");
        require(popup->Pos.y + popup->NavRectRel[0].Max.y <= popup->InnerClipRect.Max.y,
                "Removal row must fit fully inside the popup without bottom clipping");
        press_key(ImGuiKey_Enter);
        frame();
        frame();
    }
    void enable_all_y_autofit() {
        const auto* menu = ImGui::FindWindowByName("##MainMenuBar");
        const float x = menu->Pos.x + ImGui::GetStyle().WindowPadding.x +
            ImGui::CalcTextSize("新增窗口").x + 2 * ImGui::GetStyle().ItemSpacing.x +
            ImGui::CalcTextSize("设置").x * 0.5f;
        click({x, menu->Pos.y + menu->Size.y * 0.5f}); frame();
        require(popup_open(), "Settings popup must open");
        auto* popup = GImGui->OpenPopupStack.back().Window;
        const auto target = ImHashStr("Y 轴自适应", 0, popup->IDStack.back());
        move({-1000, -1000});
        for (int i = 0; i < 8 && GImGui->NavId != target; ++i) press_key(ImGuiKey_DownArrow);
        require(GImGui->NavId == target, "Global Y auto-fit action must be reachable");
        press_key(ImGuiKey_Enter); frame(); frame();
    }
    bool other_auto_y_enabled() const {
        for (int i = 0; i < GImPlot->Plots.GetMapSize(); ++i)
            if (const auto* plot = GImPlot->Plots.TryGetMapData(i); plot && plot->ID != plot_id)
                return (plot->Axes[ImAxis_Y1].Flags & ImPlotAxisFlags_AutoFit) != 0;
        throw std::runtime_error("Second panel is required");
    }
    void toggle_x_link() {
        const auto* menu = ImGui::FindWindowByName("##MainMenuBar");
        const float x = menu->Pos.x + ImGui::GetStyle().WindowPadding.x +
            ImGui::CalcTextSize("新增窗口").x + 2 * ImGui::GetStyle().ItemSpacing.x +
            ImGui::CalcTextSize("设置").x * 0.5f;
        click({x, menu->Pos.y + menu->Size.y * 0.5f});
        frame();
        require(popup_open(), "Settings popup must open");
        auto* popup = GImGui->OpenPopupStack.back().Window;
        const auto target = ImHashStr("X 轴联动", 0, popup->IDStack.back());
        for (int i = 0; i < 8 && GImGui->NavId != target; ++i) press_key(ImGuiKey_DownArrow);
        require(GImGui->NavId == target, "X link option must be reachable");
        press_key(ImGuiKey_Enter);
        press_key(ImGuiKey_Escape);
        press_key(ImGuiKey_Escape);
        frame();
    }
    void add_window_via_menu() {
        const auto* menu = ImGui::FindWindowByName("##MainMenuBar");
        require(menu != nullptr, "Adding a panel must use the visible main menu");
        // '新增窗口' is the first menu entry. Aim just inside that entry using
        // the actual menu window bounds rather than a screen-resolution guess.
        move({std::floor(menu->InnerRect.Min.x + ImGui::GetStyle().ItemSpacing.x + 1.0f),
              std::floor(menu->Pos.y + menu->Size.y * 0.5f)});
        left(true);
        left(false);
        for (int i = 0; i < 4; ++i) frame();
        require(GImPlot->Plots.GetAliveCount() == 2, "Clicking '新增窗口' must create a second panel");
    }

    PlotState other_panel_state() const {
        for (int i = 0; i < GImPlot->Plots.GetMapSize(); ++i) {
            if (const auto* plot = GImPlot->Plots.TryGetMapData(i); plot && plot->ID != plot_id)
                return {plot->PlotRect, plot->Axes[ImAxis_X1].Range, plot->Axes[ImAxis_Y1].Range};
        }
        throw std::runtime_error("Two-panel test must retain a second plot");
    }

    void register_receiver_timed() {
        store.configure(0, {{"Value", "", "v"}}, true);
        const float value=2;
        store.append_received(0,std::span<const float>(&value,1),0);
        store.append_received(1,std::span<const float>(&value,1),0.37);
        store.append_received(2,std::span<const float>(&value,1),1.2);
        frame();
    }
    void source_online(bool online) {view->set_source_connected(online); frame();}
    void small_history() {store.set_capacity_frames(20000); frame();}
    void append_small_batch() {
        const auto first = store.range().last_frame + 1;
        std::vector<float> samples(1000 * store.channels().size(), 0.25f);
        store.append(first, samples);
    }

    void append_second(std::uint64_t first_frame, float amplitude) {
        const auto channels = store.channels().size();
        std::vector<float> samples(20'000 * channels);
        constexpr double tau = 6.2831853071795864769;
        for (std::size_t i = 0; i < samples.size(); ++i)
            samples[i] = amplitude * static_cast<float>(std::sin(tau * 23.0 *
                static_cast<double>(first_frame + i / channels) / 20'000.0));
        store.append(first_frame, samples);
    }

private:
    wave::DataStore store;
    std::unique_ptr<wave::WaveView> view;
    ImGuiID plot_id{};
};

void batched_display_is_continuous_and_freezes() {
    Fixture f;
    f.add_window_via_menu();
    f.select_other_signal();
    f.toggle_overview(); f.frame();
    const auto right_edge = f.overview_outline_right();
    double previous = f.state().x.Max;
    int advances = 0;
    for (int tick = 0; tick < 240; ++tick) {
        if (tick % 3 == 0) f.append_small_batch();
        f.frame();
        const auto x = f.state().x;
        require(std::abs(f.overview_outline_right() - right_edge) < 0.05f,
                "Live overview right edge must not oscillate between batch arrival and display playhead");
        require(near(x.Max, f.other_panel_state().x.Max), "Buffered windows must share a playhead");
        require(x.Max <= f.latest_time() + 1e-6, "Buffered view cannot render future data");
        if (tick > 40) {
            require(x.Max >= previous && x.Max - previous < 0.019, "Small batches must scroll per render frame");
            if (tick % 3 != 0 && x.Max > previous) ++advances;
        }
        previous = x.Max;
    }
    require(advances > 120, "Display must advance between packets");
    f.small_history();
    // A full one-second ring rolls over every 50 ms packet. Both outline edges
    // must remain fixed while the underlying samples and time labels advance.
    for(int tick=0;tick<30;++tick) {if(tick%3==0) f.append_small_batch(); f.frame();}
    const auto stable_width=f.overview_outline_width();
    for(int tick=0;tick<120;++tick) {
        if(tick%3==0) f.append_small_batch();
        f.frame();
        require(std::abs(f.overview_outline_width()-stable_width)<0.05f,
                "Full-buffer overview must keep a stable scale between packets");
    }
    f.press_space();
    const auto frozen = f.state().x;
    for (int tick = 0; tick < 30; ++tick) {
        if (tick % 3 == 0) f.append_small_batch();
        f.frame();
        require(near(f.state().x.Max, frozen.Max), "Background buffering must not move paused axes");
    }
    f.press_space();
    require(f.state().x.Max > frozen.Max, "Resume must return to the live playhead");
    for (int tick = 0; tick < 60; ++tick) f.frame();
    require(near(f.state().x.Max, f.latest_time()), "Disconnected display must drain to real data end");
    std::cout << "PASS batch smoothing between packets, shared playhead, freeze/resume and stream end\n";
}

void remove_windows_and_recreate() {
    Fixture f;
    f.add_window_via_menu();
    require(f.active_panel_count() == 2, "Two active panels must be visible");
    f.remove_first_window();
    require(f.active_panel_count() == 1, "Clear window must delete only one panel");
    f.remove_first_window();
    require(f.active_panel_count() == 0 && f.points_drawn() == 0,
            "Removing the last panel must leave an empty canvas");
    const auto* menu = ImGui::FindWindowByName("##MainMenuBar");
    f.click({menu->InnerRect.Min.x + ImGui::GetStyle().ItemSpacing.x + 1,
             menu->Pos.y + menu->Size.y * 0.5f});
    f.frame();
    require(f.active_panel_count() == 1, "New window must work from an empty canvas");
    std::cout << "PASS deleting individual and last windows, then creating a new window\n";
}
void paused_x_link_is_global_and_optional() {
    Fixture f;
    f.add_window_via_menu();
    f.select_other_signal();
    f.press_space();
    const auto other_y = f.other_panel_state().y;
    const auto press = f.press_point();
    f.move(press);
    f.left(true);
    f.move({press.x - 45, press.y + 20});
    f.left(false);
    f.frame();
    auto first = f.state(), other = f.other_panel_state();
    require(near(first.x.Min, other.x.Min) && near(first.x.Max, other.x.Max),
            "Paused X link must be enabled by default for all windows");
    require(near(other.y.Min, other_y.Min) && near(other.y.Max, other_y.Max),
            "Linking X must leave other windows' Y axes unchanged");
    f.wheel(2);
    f.frame();
    require(near(f.state().x.Min, f.other_panel_state().x.Min) && near(f.state().x.Max, f.other_panel_state().x.Max),
            "Paused wheel zoom must link both X endpoints");
    f.toggle_x_link();
    const auto detached = f.other_panel_state();
    f.move(f.press_point());
    f.wheel(2);
    f.frame();
    require(near(f.other_panel_state().x.Min, detached.x.Min) && near(f.other_panel_state().x.Max, detached.x.Max),
            "Disabling X link must restore independent inspection");
    const auto before = f.state();
    f.toggle_x_link();
    require(near(f.state().x.Min, before.x.Min) && near(f.other_panel_state().x.Min, detached.x.Min),
            "Enabling X link must not immediately jump either window");
    f.toggle_overview();
    f.click(f.overview_fraction_point(0.4f));
    f.frame();
    require(near(f.state().x.Min, f.other_panel_state().x.Min) && near(f.state().x.Max, f.other_panel_state().x.Max),
            "Overview navigation must also link all paused windows");
    f.press_space();
    f.frame();
    require(near(f.state().x.Max, f.latest_time()) && near(f.other_panel_state().x.Max, f.latest_time()),
            "Resuming must follow latest data for both windows");
    std::cout << "PASS default paused X link, independent Y, opt-out, no jump on enable and linked overview\n";
}
void axis_label_drag_and_wheel() {
    Fixture f;
    f.add_window_via_menu();
    auto initial = f.state();
    auto point = f.y_axis_point(false);
    f.move(point); f.left(true);
    f.move({point.x + 80, point.y + 30}); f.left(false); f.frame();
    auto y_pan = f.state();
    require(near(y_pan.x.Min, initial.x.Min) && near(y_pan.x.Max, initial.x.Max) &&
            y_pan.y.Min > initial.y.Min && near(y_pan.y.Size(), initial.y.Size()),
            "Y-label diagonal drag must pan only Y, even after entering the plot");
    point = f.x_axis_point();
    f.move(point); f.wheel(1); f.frame();
    auto x_zoom = f.state();
    require(x_zoom.x.Size() < y_pan.x.Size() && near(x_zoom.y.Min, y_pan.y.Min) && near(x_zoom.y.Max, y_pan.y.Max),
            "X-label wheel must zoom only X");
    point = f.x_axis_point();
    f.move(point); f.left(true);
    f.move({point.x + 40, point.y - 50});
    f.append_second(20000, 5); f.frame();
    require(near(f.state().x.Max, f.latest_time()) && near(f.state().x.Size(), x_zoom.x.Size()),
            "Live X-label dragging must continue following new data while held");
    f.left(false); f.frame();
    f.append_second(40000, 5); f.frame();
    require(near(f.state().x.Max, f.latest_time()) && near(f.state().y.Min, x_zoom.y.Min),
            "Releasing an X-label drag must not freeze data or move Y");
    f.press_space();
    x_zoom = f.state();
    point = f.x_axis_point();
    f.move(point); f.left(true);
    f.move({point.x + 40, point.y - 50}); f.left(false); f.frame();
    auto x_pan = f.state();
    require(x_pan.x.Min < x_zoom.x.Min && near(x_pan.x.Size(), x_zoom.x.Size()) &&
            near(x_pan.y.Min, x_zoom.y.Min) && near(x_pan.y.Max, x_zoom.y.Max),
            "Explicitly paused X-label diagonal drag must pan only X");
    require(near(f.other_panel_state().x.Min, x_pan.x.Min) && near(f.other_panel_state().x.Max, x_pan.x.Max),
            "Axis dragging must participate in global paused X linking");
    f.append_second(60000, 5); f.frame();
    require(near(f.state().x.Min, x_pan.x.Min), "X-axis pan must remain frozen while new data arrives");
    f.move(f.press_point());
    const auto before = f.state();
    f.wheel(1); f.frame();
    require(f.state().x.Size() < before.x.Size() && f.state().y.Size() < before.y.Size(),
            "Plot-area wheel must zoom both axes");
    std::cout << "PASS axis-label drag/wheel preserves live mode, explicit pause pans X, linking and plot XY wheel\n";
}
void live_x_link_tracks_zoom_and_new_data() {
    Fixture f;
    f.add_window_via_menu();
    f.select_other_signal();
    const auto other_y = f.other_panel_state().y;
    auto check = [&]() {
        f.frame();
        const auto a = f.state(), b = f.other_panel_state();
        require(near(a.x.Min, b.x.Min) && near(a.x.Max, b.x.Max) && near(a.x.Max, f.latest_time()),
                "Live X edits must synchronize both windows while following latest data");
        require(near(b.y.Min, other_y.Min) && near(b.y.Max, other_y.Max),
                "Live X linking must not modify another window's Y range");
    };
    f.move(f.x_axis_point()); f.wheel(2); check();
    f.append_second(20000, 1); check();
    auto point = f.press_point();
    f.move(point); f.right(true); f.move({point.x + 40, point.y}); f.right(false); check();
    // The other window must be able to drive the link too.
    point = f.other_panel_state().rect.GetCenter();
    f.move(point); f.right(true); f.move({point.x + 30, point.y}); f.right(false); check();
    f.toggle_overview(); f.click(f.overview_fraction_point(0.5f)); check();
    f.toggle_x_link();
    const auto detached = f.other_panel_state().x.Size();
    f.move(f.x_axis_point()); f.wheel(2); f.frame();
    require(near(f.other_panel_state().x.Size(), detached) && !near(f.state().x.Size(), detached),
            "Disabling X link must permit independent live zoom");
    f.toggle_x_link();
    f.append_second(40000, 1); f.frame(); f.frame();
    require(near(f.other_panel_state().x.Size(), detached),
            "Live data arrival alone must not trigger synchronization after enabling");
    f.move(f.x_axis_point()); f.wheel(1); check();
    std::cout << "PASS live X link via wheel, either window, right drag and overview; opt-out and data arrival\n";
}
void axis_right_drag_locks_axis_and_keeps_arrow() {
    Fixture f;
    f.add_window_via_menu();
    auto point = f.x_axis_point();
    f.move(point);
    require(ImGui::GetMouseCursor() == ImGuiMouseCursor_Arrow, "X labels must keep the normal cursor");
    auto initial = f.state();
    f.right(true); f.move({point.x + 45, point.y - 40}); f.right(false); f.frame();
    auto zoomed = f.state();
    require(zoomed.x.Size() < initial.x.Size() && near(zoomed.y.Min, initial.y.Min) && near(zoomed.y.Max, initial.y.Max),
            "Right dragging X labels must zoom only X even after leaving the labels");
    require(near(f.other_panel_state().x.Size(), zoomed.x.Size()), "Axis right zoom must participate in X linking");
    point = f.y_axis_point(false);
    f.move(point);
    require(ImGui::GetMouseCursor() == ImGuiMouseCursor_Arrow, "Y labels must keep the normal cursor");
    initial = f.state();
    f.right(true); f.move({point.x + 50, point.y - 35}); f.right(false); f.frame();
    zoomed = f.state();
    require(zoomed.y.Size() < initial.y.Size() && near(zoomed.x.Min, initial.x.Min) && near(zoomed.x.Max, initial.x.Max),
            "Right dragging Y labels must zoom only Y");
    require(!f.popup_open(), "Axis right drag release must not open a menu");
    std::cout << "PASS axis right zoom locks X/Y, links X and retains the arrow cursor\n";
}
void independent_y_autofit_and_manual_override() {
    Fixture f;
    require(f.auto_y_enabled(), "Initial window must default to Y auto-fit");
    f.add_window_via_menu();
    f.toggle_auto_y();
    require(!f.auto_y_enabled(), "Menu must disable only the current window's Y auto-fit");
    const auto held = f.state();
    f.move(f.x_axis_point()); f.wheel(1); f.frame();
    require(!f.auto_y_enabled() && near(f.state().y.Min, held.y.Min), "X zoom must preserve manual Y state");
    f.toggle_auto_y();
    f.press_space();
    require(f.auto_y_enabled(), "Freezing must retain Y auto-fit");
    auto point = f.x_axis_point();
    f.move(point); f.left(true); f.move({point.x + 25, point.y - 30}); f.left(false); f.frame();
    require(f.auto_y_enabled(), "X-only dragging must retain Y auto-fit");
    point = f.y_axis_point(false);
    f.move(point); f.left(true); f.move({point.x + 30, point.y + 25}); f.left(false); f.frame();
    require(!f.auto_y_enabled(), "Manual Y dragging must exit Y auto-fit");
    f.press_space();
    require(!f.auto_y_enabled(), "Resume must retain manual Y state");
    f.toggle_auto_y();
    f.append_second(20000, 20); f.frame(); f.frame();
    require(f.auto_y_enabled() && f.state().y.Size() > 30, "Re-enabling must fit newly arriving visible data");
    f.click(f.legend_point());
    f.append_second(40000, 100); f.frame(); f.frame();
    require(f.state().y.Size() < 100, "Hidden signals must not participate in Y auto-fit");
    std::cout << "PASS default independent Y auto-fit, menu toggle, X preservation, manual Y exit and hidden signal exclusion\n";
}
void held_x_zoom_continuously_renders_y_autofit() {
    Fixture f;
    auto point = f.x_axis_point();
    f.move(point); f.right(true); f.move({point.x + 40, point.y});
    f.append_second(20000, 20);
    for (int i = 0; i < 4; ++i) f.frame();
    require(ImGui::IsMouseDown(ImGuiMouseButton_Right) && f.auto_y_enabled(),
            "X drag must still be held with auto-fit enabled");
    require(f.rendered_y_tick_extent() > 10,
            "Rendered Y ticks must expand while X right drag remains held, not only after release");
    f.append_second(40000, 0.2f);
    for (int i = 0; i < 4; ++i) f.frame();
    require(f.rendered_y_tick_extent() < 2,
            "Rendered Y ticks must also shrink as smaller data becomes visible during a held X drag");
    f.right(false); f.frame();
    require(f.auto_y_enabled(), "Release must preserve Y auto-fit");
    std::cout << "PASS held X right zoom continuously expands and shrinks rendered Y ticks before release\n";
}
void overview_context_can_close_and_reopen() {
    Fixture f;
    f.toggle_overview();
    const auto before = f.state();
    f.move(f.overview_fraction_point(0.5f)); f.right(true); f.right(false); f.frame();
    require(f.popup_open(), "Right click on overview must open its own menu");
    auto* popup = GImGui->OpenPopupStack.back().Window;
    require(popup != nullptr, "Overview popup must be visible");
    const auto target = ImHashStr("Zoom", 0, popup->IDStack.back());
    f.move({-1000, -1000});
    for (int i = 0; i < 5 && GImGui->NavId != target; ++i) f.press_key(ImGuiKey_DownArrow);
    require(GImGui->NavId == target, "Overview menu must expose the shared Zoom switch");
    const auto rect = popup->NavRectRel[0];
    f.click({popup->Pos.x + rect.GetCenter().x, popup->Pos.y + rect.GetCenter().y});
    f.frame();
    require(f.overview_window() == nullptr && !f.popup_open(), "Unchecking must close both overview and its popup");
    require(near(f.state().x.Min, before.x.Min) && near(f.state().x.Max, before.x.Max) &&
            near(f.state().y.Min, before.y.Min) && near(f.state().y.Max, before.y.Max),
            "Closing overview must preserve the main plot ranges");
    f.toggle_overview();
    require(f.overview_window() != nullptr, "Main window Zoom switch must reflect closure and reopen overview");
    f.append_second(20000, 1); f.frame();
    require(near(f.state().x.Max, f.latest_time()), "Overview menu must not freeze live data");
    std::cout << "PASS overview right-click Zoom switch closes preview and shares state with main menu\n";
}
void clearing_buffer_preserves_windows_and_pause() {
    Fixture f;
    f.add_window_via_menu();
    const auto before = f.state();
    f.clear_buffer_via_menu();
    require(f.points_drawn() == 0 && f.active_panel_count() == 2,
            "Clearing must remove samples while preserving all windows");
    require(near(f.state().x.Size(), before.x.Size()), "Clear must preserve the chosen time span");
    f.append_second(20000, 1); f.frame(); f.frame();
    require(f.points_drawn() > 0 && near(f.state().x.Max, f.latest_time()),
            "Live clear must permit new data to appear without resetting the producer timeline");
    f.press_space();
    f.clear_buffer_via_menu();
    require(f.points_drawn() == 0, "Frozen clear must also discard the shared snapshot");
    const auto frozen = f.state();
    f.append_second(40000, 5); f.frame(); f.frame();
    require(f.points_drawn() == 0 && near(f.state().x.Min, frozen.x.Min),
            "Clearing while frozen must remain frozen even as the live store receives new data");
    f.press_space(); f.frame();
    require(f.points_drawn() > 0 && near(f.state().x.Max, f.latest_time()) && f.active_panel_count() == 2,
            "Resume after clear must show new live data in the existing windows");
    std::cout << "PASS shared buffer clear live/frozen, retained windows, continued reception and resume\n";
}
void local_clear_keeps_other_windows_and_shared_history() {
    Fixture f;
    f.add_window_via_menu();
    f.select_other_signal();
    f.press_space();
    const auto other = f.other_panel_state();
    const auto before = f.points_drawn();
    f.clear_buffer_via_menu(true);
    require(f.points_drawn() > 0 && f.points_drawn() < before,
            "Local clear must hide only one window while the other still draws the snapshot");
    require(near(f.other_panel_state().x.Min, other.x.Min) && near(f.other_panel_state().x.Max, other.x.Max),
            "Local clear must not change other windows even with X linking enabled");
    f.select_other_signal(); // Hide the second panel to inspect the cleared first panel alone.
    f.toggle_overview();
    require(f.points_drawn() == 0, "Local clear must also hide old samples from its overview");
    f.append_second(20000, 1); f.frame(); f.frame();
    require(f.points_drawn() == 0, "Locally cleared frozen window must stay empty as data arrives");
    f.press_space(); f.frame();
    require(f.points_drawn() > 0 && near(f.state().x.Max, f.latest_time()),
            "Resume must display samples received after local clear");
    f.clear_buffer_via_menu(true);
    require(f.points_drawn() == 0, "Live local clear must immediately hide its history");
    f.append_second(40000, 1); f.frame(); f.frame();
    require(f.points_drawn() > 0, "Live local clear must continue displaying new samples");
    std::cout << "PASS local clear preserves shared history and other windows; filters main/overview and resumes new data\n";
}
void new_windows_inherit_linked_x_immediately() {
    for (bool frozen : {false, true}) {
        Fixture f;
        f.move(f.x_axis_point()); f.wheel(3); f.frame();
        if (frozen) {
            f.press_space();
            const auto press = f.x_axis_point();
            f.move(press); f.left(true); f.move({press.x + 90, press.y}); f.left(false); f.frame();
        }
        const auto original = f.state();
        f.add_window_via_menu();
        auto added = f.other_panel_state();
        require(near(added.x.Min, original.x.Min) && near(added.x.Max, original.x.Max),
                "New linked window must immediately inherit the current zoom and history position");
        f.select_other_signal();
        added = f.other_panel_state();
        require(near(added.x.Min, f.state().x.Min) && near(added.x.Max, f.state().x.Max),
                "Selecting new signals must preserve inherited X alignment without another axis gesture");
        f.append_second(20000, 1); f.frame(); f.frame();
        require(near(f.state().x.Min, f.other_panel_state().x.Min) && near(f.state().x.Max, f.other_panel_state().x.Max),
                "New windows must remain aligned as data arrives, live or frozen");
    }
    Fixture independent;
    independent.toggle_x_link();
    independent.move(independent.x_axis_point()); independent.wheel(3); independent.frame();
    independent.add_window_via_menu();
    require(near(independent.other_panel_state().x.Size(), 0.2) &&
            !near(independent.state().x.Size(), independent.other_panel_state().x.Size()),
            "With linking disabled a new window must retain its independent default span");
    std::cout << "PASS new linked windows immediately inherit live/frozen X range and retain independent defaults when disabled\n";
}
void global_y_autofit_is_a_one_shot_action() {
    Fixture f;
    f.add_window_via_menu();
    f.select_other_signal();
    f.move(f.press_point()); f.wheel(1); f.frame();
    f.move(f.other_panel_state().rect.GetCenter()); f.wheel(1); f.frame();
    require(!f.auto_y_enabled() && !f.other_auto_y_enabled(), "Both panels must start with manual Y");
    const auto a = f.state().x, b = f.other_panel_state().x;
    f.enable_all_y_autofit();
    require(f.auto_y_enabled() && f.other_auto_y_enabled(), "Settings action must enable every existing panel");
    require(near(f.state().x.Min, a.Min) && near(f.state().x.Max, a.Max) &&
            near(f.other_panel_state().x.Min, b.Min) && near(f.other_panel_state().x.Max, b.Max),
            "Batch Y auto-fit must not alter X views");
    f.move(f.y_axis_point(false)); f.wheel(1); f.frame();
    require(!f.auto_y_enabled() && f.other_auto_y_enabled(), "Manual Y editing must remain independent after batch action");
    f.press_space(); f.enable_all_y_autofit();
    require(f.auto_y_enabled() && f.other_auto_y_enabled(), "Batch action must also work while frozen");
    const auto frozen = f.state().x;
    f.append_second(20000, 2); f.frame();
    require(near(f.state().x.Max, frozen.Max), "Batch action must not resume frozen data");
    std::cout << "PASS one-shot global Y auto-fit preserves independent controls, X views and frozen state\n";
}
void right_drag_zoom_and_anchor() {
    Fixture f;
    const ImVec2 press = f.press_point();
    f.move(press);
    f.right(true);
    const auto initial = f.state();
    const double y_fraction = 1.0 - (press.y - initial.rect.Min.y) / initial.rect.GetHeight();
    const double y_anchor = initial.y.Min + y_fraction * initial.y.Size();

    f.move({press.x + 40.0f, press.y - 25.0f});
    const auto half_drag = f.state();
    f.move({press.x + 80.0f, press.y - 50.0f});
    f.frame();
    const auto zoomed = f.state();
    require(zoomed.x.Size() < initial.x.Size() * 0.99,
            "Rightward right-button drag must zoom in continuously on X");
    require(zoomed.y.Size() < initial.y.Size() * 0.99,
            "Upward right-button drag must zoom in continuously on Y");
    require(near(zoomed.x.Max, f.latest_time()),
            "Live right drag must anchor X to the newest sample rather than the mouse");
    require(near((y_anchor - zoomed.y.Min) / zoomed.y.Size(), y_fraction),
            "Y zoom must retain the press-time anchor ratio, not the moving cursor or centre");
    std::cout << "PASS live right drag zooms X from the latest edge and Y from the mouse anchor\n";

    // Feed later, much taller data while the right button remains held.
    // Following must update even when the pointer has stopped moving.
    f.append_second(20'000, 20.0f);
    f.frame();
    f.frame();
    const auto held = f.state();
    require(near(held.x.Max, f.latest_time()) && near(held.x.Size(), zoomed.x.Size()),
            "Held right drag must follow new data without accumulating additional X zoom");
    require(near(held.y.Min, zoomed.y.Min) && near(held.y.Max, zoomed.y.Max),
            "Held right drag must retain manual Y limits when taller data arrives");
    f.move({press.x + 40.0f, press.y - 25.0f});
    require(near(f.state().x.Size(), half_drag.x.Size()) && near(f.state().x.Max, f.latest_time()),
            "Live drag span must use total displacement from the press, not accumulated per-frame zoom");
    f.move({press.x + 80.0f, press.y - 50.0f});
    require(near(f.state().x.Size(), zoomed.x.Size()),
            "Returning to the same drag displacement must restore the same X span");
    std::cout << "PASS held right drag follows new data and uses press-time total displacement\n";

    f.right(false);
    f.frame();
    require(!f.popup_open(), "Releasing after right drag must not open any context menu");
    const auto released = f.state();
    require(near(released.x.Size(), zoomed.x.Size()) && near(released.x.Max, f.latest_time()) &&
            near(released.y.Min, zoomed.y.Min) && near(released.y.Max, zoomed.y.Max),
            "Mouse release must preserve the live X span and manual Y limits");
    f.append_second(40'000, 40.0f);
    f.frame();
    f.frame();
    const auto later = f.state();
    require(near(later.x.Size(), released.x.Size()) && near(later.x.Max, f.latest_time()) &&
            near(later.y.Min, released.y.Min) && near(later.y.Max, released.y.Max),
            "Released right drag must continue following with its chosen X span and manual Y range");
    require(!f.popup_open(), "Context menu must remain closed after drag and new data");
    std::cout << "PASS drag release suppresses menu and continues live following\n";
}

void nonlive_right_drag_retains_mouse_anchors() {
    Fixture f;
    f.press_space();
    const auto press = f.press_point();
    f.move(press);
    f.right(true);
    const auto initial = f.state();
    const double x_fraction = (press.x - initial.rect.Min.x) / initial.rect.GetWidth();
    const double y_fraction = 1.0 - (press.y - initial.rect.Min.y) / initial.rect.GetHeight();
    const double x_anchor = initial.x.Min + x_fraction * initial.x.Size();
    const double y_anchor = initial.y.Min + y_fraction * initial.y.Size();
    f.move({press.x + 80.0f, press.y - 50.0f});
    f.frame();
    const auto zoomed = f.state();
    require(zoomed.x.Size() < initial.x.Size() * 0.99 && zoomed.y.Size() < initial.y.Size() * 0.99,
            "Non-live right drag must zoom both axes");
    require(near((x_anchor - zoomed.x.Min) / zoomed.x.Size(), x_fraction) &&
            near((y_anchor - zoomed.y.Min) / zoomed.y.Size(), y_fraction),
            "Paused right drags must retain both press-time mouse anchor ratios");
    f.append_second(20'000, 20.0f);
    f.frame();
    const auto held = f.state();
    require(near(held.x.Min, zoomed.x.Min) && near(held.x.Max, zoomed.x.Max) &&
            near(held.y.Min, zoomed.y.Min) && near(held.y.Max, zoomed.y.Max),
            "Non-live right drag must retain its mouse-anchored ranges when new data arrives while held");
    f.right(false);
    f.frame();
    const auto later = f.state();
    require(near(later.x.Min, zoomed.x.Min) && near(later.x.Max, zoomed.x.Max) &&
            near(later.y.Min, zoomed.y.Min) && near(later.y.Max, zoomed.y.Max),
            "Non-live right drag must not resume live following while held or after release");
    require(!f.popup_open(), "Non-live right drag must not open a menu on release");
    std::cout << "PASS paused right drag retains both mouse anchors and remains paused\n";
}

void live_left_pan_keeps_following() {
    Fixture f;
    const auto press = f.press_point();
    f.move(press);
    f.left(true);
    const auto initial = f.state();
    f.move({press.x + 80.0f, press.y + 50.0f});
    f.frame();
    const auto panned = f.state();
    require(near(panned.x.Min, initial.x.Min) && near(panned.x.Max, initial.x.Max),
            "Live left drag must ignore horizontal displacement and retain the X time span");
    require(panned.y.Min > initial.y.Min + 0.001 && near(panned.y.Size(), initial.y.Size()),
            "Live left drag must translate Y without changing its scale");
    require(near(panned.y.Min - initial.y.Min, panned.y.Max - initial.y.Max),
            "Live Y pan must translate both Y limits equally");

    f.append_second(20'000, 20.0f);
    f.frame();
    f.frame();
    const auto held = f.state();
    require(near(held.x.Max, f.latest_time()) && near(held.x.Size(), initial.x.Size()),
            "Held left drag must keep following new data at the original X span");
    require(near(held.y.Min, panned.y.Min) && near(held.y.Max, panned.y.Max),
            "Held left drag must retain its Y offset instead of autofitting taller live data");
    f.left(false);
    f.frame();
    const auto released = f.state();
    require(near(released.x.Max, held.x.Max) && near(released.x.Size(), held.x.Size()) &&
            near(released.y.Min, held.y.Min) && near(released.y.Max, held.y.Max),
            "Releasing a live left drag must not jump either axis back");
    require(!f.popup_open(), "Left drag release must not open any context menu");

    f.append_second(40'000, 40.0f);
    f.frame();
    f.frame();
    const auto later = f.state();
    require(near(later.x.Max, f.latest_time()) && near(later.x.Size(), initial.x.Size()),
            "Left drag must preserve live following after the mouse is released");
    require(near(later.y.Min, panned.y.Min) && near(later.y.Max, panned.y.Max),
            "Released left drag must preserve the selected Y offset across later data");
    std::cout << "PASS live left pan keeps X following while retaining its Y offset during and after the drag\n";
}

void nonlive_left_pan_translates_both_axes() {
    Fixture f;
    f.press_space();
    const auto press = f.press_point();
    f.move(press);
    f.left(true);
    const auto initial = f.state();
    f.move({press.x + 80.0f, press.y + 50.0f});
    f.frame();
    const auto panned = f.state();
    require(panned.x.Max < initial.x.Max - 0.001 && near(panned.x.Size(), initial.x.Size()),
            "Paused left drag must freely translate X without changing its scale");
    require(panned.y.Min > initial.y.Min + 0.001 && near(panned.y.Size(), initial.y.Size()),
            "Paused left drag must freely translate Y without changing its scale");
    f.append_second(20'000, 20.0f);
    f.frame();
    const auto held = f.state();
    require(near(held.x.Min, panned.x.Min) && near(held.x.Max, panned.x.Max) &&
            near(held.y.Min, panned.y.Min) && near(held.y.Max, panned.y.Max),
            "Held non-live left drag must ignore later background data");
    f.left(false);
    f.frame();
    f.append_second(40'000, 40.0f);
    f.frame();
    f.frame();
    const auto later = f.state();
    require(near(later.x.Min, panned.x.Min) && near(later.x.Max, panned.x.Max) &&
            near(later.y.Min, panned.y.Min) && near(later.y.Max, panned.y.Max),
            "Released non-live left drag must preserve both ranges instead of resuming live following");
    require(!f.popup_open(), "Non-live left drag must not open a context menu");
    std::cout << "PASS paused left pan freely moves both axes and stays paused\n";
}

void right_click_opens_menu() {
    Fixture f;
    f.move(f.press_point());
    f.right(true);
    require(!f.popup_open(), "Right menu must not open on mouse-down");
    f.right(false);
    f.frame();
    require(f.popup_open(), "A stationary right click must open the context menu");
    std::cout << "PASS stationary right click opens menu\n";
}

void returning_to_press_point_still_suppresses_menu() {
    Fixture f;
    const auto press = f.press_point();
    f.move(press);
    f.right(true);
    const auto initial = f.state();
    f.move({press.x + 80.0f, press.y - 50.0f});
    f.frame();
    require(f.state().x.Size() < initial.x.Size() * 0.99,
            "Out-and-back gesture must actually enter zoom mode before returning");
    f.move(press);
    f.frame();
    f.right(false);
    f.frame();
    require(!f.popup_open(),
            "A drag that exceeded the threshold must not become a click after returning to its origin");
    std::cout << "PASS out-and-back drag never opens menu\n";
}

void legend_click_hides_signal_without_panning() {
    Fixture f;
    require(f.signal_shown(), "Signal must initially be visible");
    f.move(f.legend_point());
    const auto initial = f.state();
    f.left(true);
    f.left(false);
    f.frame();
    require(!f.signal_shown(), "Ordinary left click on the legend must hide its signal");
    const auto hidden = f.state();
    require(near(hidden.x.Min, initial.x.Min) && near(hidden.x.Max, initial.x.Max) &&
            near(hidden.y.Min, initial.y.Min) && near(hidden.y.Max, initial.y.Max),
            "Clicking the legend must not pan or reset either axis");
    require(!f.popup_open(), "Left-clicking a legend entry must not open a context menu");
    std::cout << "PASS legend click hides its signal without panning\n";
}

void legend_double_click_does_not_freeze() {
    Fixture f;
    const auto initial = f.state();
    f.double_click(f.legend_point());
    f.append_second(20'000, 20.0f);
    f.frame();
    f.frame();
    const auto later = f.state();
    require(near(later.x.Max, f.latest_time()) && near(later.x.Size(), initial.x.Size()),
            "Double-clicking the legend must not freeze the live view or change its X span");
    require(!f.popup_open(), "Double-clicking the legend must not open a context menu");
    std::cout << "PASS legend double click leaves live following active\n";
}

void double_click_freeze_resume_preserves_view(bool interact_while_frozen) {
    Fixture f;
    const auto initial = f.state();
    f.double_click(f.press_point());
    const auto frozen = f.state();
    require(near(frozen.x.Min, initial.x.Min) && near(frozen.x.Max, initial.x.Max) &&
            near(frozen.y.Min, initial.y.Min) && near(frozen.y.Max, initial.y.Max),
            "Double-click freeze must retain the currently displayed ranges");
    f.append_second(20'000, 20.0f);
    f.frame();
    f.frame();
    const auto with_background_data = f.state();
    require(near(with_background_data.x.Min, frozen.x.Min) && near(with_background_data.x.Max, frozen.x.Max) &&
            near(with_background_data.y.Min, frozen.y.Min) && near(with_background_data.y.Max, frozen.y.Max),
            "Frozen ranges must ignore later, taller samples appended in the background");

    if (interact_while_frozen) {
        const auto pan_press = f.press_point();
        f.move(pan_press);
        f.left(true);
        f.move({std::floor(pan_press.x + frozen.rect.GetWidth() * 2.0f), pan_press.y + 30.0f});
        f.left(false);
        f.frame();
        const auto panned = f.state();
        require(panned.x.Max < frozen.x.Min && panned.y.Min > frozen.y.Min + 0.001,
                "After double-click freeze, left drag must freely translate both X and Y");
        require(f.points_drawn() > 0,
                "Double-click freeze must retain the full cached pool, including samples outside the initial view");
        const auto press = f.press_point();
        f.move(press);
        f.right(true);
        f.move({press.x + 80.0f, press.y - 50.0f});
        f.right(false);
        f.frame();
        require(f.state().x.Size() < panned.x.Size() * 0.99 &&
                f.state().y.Size() < panned.y.Size() * 0.99,
                "Right drag must remain usable after double-click freeze");
        const auto cursor = f.press_point();
        f.move(cursor);
        const auto before_wheel = f.state();
        const double fraction = (cursor.x - before_wheel.rect.Min.x) / before_wheel.rect.GetWidth();
        const double anchor = before_wheel.x.Min + fraction * before_wheel.x.Size();
        f.wheel(1.0f);
        f.frame();
        require(f.state().x.Size() < before_wheel.x.Size() * 0.99 &&
                near((anchor - f.state().x.Min) / f.state().x.Size(), fraction),
                "Wheel after double-click freeze must zoom around the mouse anchor");
        require(f.state().y.Size() < before_wheel.y.Size(),
                "Plot wheel must zoom Y as well as X");
    }
    const auto chosen = f.state();
    require(f.points_drawn() > 0, "Frozen view must initially display data from its retained interval");
    // Advancing the five-second live ring to seven seconds evicts the original
    // interval. The global snapshot must still render its retained old points.
    for (std::uint64_t second = 2; second <= 6; ++second)
        f.append_second(second * 20'000, 20.0f);
    f.frame();
    f.frame();
    const auto evicted = f.state();
    require(near(evicted.x.Min, chosen.x.Min) && near(evicted.x.Max, chosen.x.Max) &&
            near(evicted.y.Min, chosen.y.Min) && near(evicted.y.Max, chosen.y.Max),
            "Frozen view must retain its ranges even after the live ring evicts the original data");
    require(f.points_drawn() > 0,
            "Double-click freeze must preserve old samples after they disappear from the live ring");
    f.double_click(f.press_point());
    const auto resumed = f.state();
    require(near(resumed.x.Max, f.latest_time()) && near(resumed.x.Size(), chosen.x.Size()),
            "Double-click resume must follow the newest sample using the currently selected X span");
    require(interact_while_frozen ? near(resumed.y.Min, chosen.y.Min) && near(resumed.y.Max, chosen.y.Max) : resumed.y.Size() > chosen.y.Size() * 5,
            "Resume must preserve manual Y or retain enabled auto-fit");
    require(f.points_drawn() > 0, "Resuming must render the newest live data again");

    f.append_second(140'000, 40.0f);
    f.frame();
    f.frame();
    const auto later = f.state();
    require(near(later.x.Max, f.latest_time()) && near(later.x.Size(), chosen.x.Size()),
            "Resumed view must keep following new data at the preserved X span");
    require(interact_while_frozen ? near(later.y.Min, chosen.y.Min) && near(later.y.Max, chosen.y.Max) : later.y.Size() > resumed.y.Size() * 1.5,
            "Resume must preserve the prior Y auto-fit setting");
    require(!f.popup_open(), "Double-click freeze and resume must not open a context menu");
    std::cout << (interact_while_frozen ? "PASS double-click freeze retains old data, allows XY pan/right drag/wheel, and resumes at the chosen scale\n" :
                                        "PASS double-click freeze survives ring eviction and resumes without resetting Y\n");
}

void double_click_freezes_and_resumes_all_panels() {
    Fixture f;
    f.add_window_via_menu();
    f.select_other_signal();
    const auto first = f.state();
    const auto second = f.other_panel_state();
    f.double_click(f.press_point());
    f.append_second(20'000, 20.0f);
    f.frame();
    f.frame();
    require(near(f.state().x.Min, first.x.Min) && near(f.state().x.Max, first.x.Max),
            "Double click must freeze the targeted panel");
    const auto other = f.other_panel_state();
    require(near(other.x.Min, second.x.Min) && near(other.x.Max, second.x.Max),
            "Double-click freeze must stop all panels rather than only the clicked panel");
    const auto rect = other.rect;
    f.double_click({std::floor(rect.Min.x + rect.GetWidth() * 0.37f),
                    std::floor(rect.Min.y + rect.GetHeight() * 0.62f)});
    f.append_second(40'000, 40.0f);
    f.frame();
    f.frame();
    require(near(f.state().x.Max, f.latest_time()) && near(f.other_panel_state().x.Max, f.latest_time()),
            "Double clicking either frozen panel must restore updates and following for both panels");
    require(near(f.state().x.Size(), first.x.Size()) && near(f.other_panel_state().x.Size(), second.x.Size()),
            "Global double-click resume must retain each panel's time span");
    std::cout << "PASS double click freezes all panels and a double click in either panel resumes all\n";
}

void live_wheel_changes_x_span_and_keeps_following() {
    Fixture f;
    f.move(f.press_point());
    const auto initial = f.state();
    require(near(initial.x.Max, f.latest_time()), "Fixture must initially follow the latest sample");
    f.wheel(1.0f);
    f.frame();
    const auto zoomed = f.state();
    require(zoomed.x.Size() < initial.x.Size() * 0.99,
            "Wheel in the plot interior must zoom X while following live data");
    require(near(zoomed.x.Max, f.latest_time()),
            "Live X wheel zoom must keep the newest sample at the right edge immediately");

    f.append_second(20'000, 20.0f);
    f.frame();
    f.frame();
    const auto later = f.state();
    require(near(later.x.Max, f.latest_time()) && later.x.Max > zoomed.x.Max + 0.9,
            "X wheel zoom must continue following newly appended data");
    require(near(later.x.Size(), zoomed.x.Size()),
            "Following new data must retain the time span selected by the wheel");
    require(near(later.y.Min, zoomed.y.Min) && near(later.y.Max, zoomed.y.Max),
            "Plot wheel zoom must preserve its manual Y range for later taller data");
    require(!f.popup_open(), "Wheel zoom must not open a context menu");
    std::cout << "PASS live wheel changes X span and keeps following new data\n";
}

void y_axis_wheel_keeps_x_following(bool inside_plot) {
    Fixture f;
    f.move(f.y_axis_point(inside_plot));
    if (inside_plot) ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
    const auto initial = f.state();
    f.wheel(1.0f);
    f.frame();
    const auto zoomed = f.state();
    require(zoomed.y.Size() < initial.y.Size() * 0.99,
            inside_plot ? "Wheel 20 px inside the left plot edge must zoom Y" :
                          "Wheel over the left Y-axis label margin must zoom Y");
    require(near(zoomed.x.Min, initial.x.Min) && near(zoomed.x.Max, initial.x.Max),
            "Y-axis wheel zoom must leave both X limits unchanged");

    // Taller future data distinguishes retained manual Y limits from an
    // accidental autofit, while its later timestamp proves X still follows.
    f.append_second(20'000, 20.0f);
    f.frame();
    f.frame();
    const auto later = f.state();
    require(near(later.x.Max, f.latest_time()) && near(later.x.Size(), initial.x.Size()),
            "Y-axis wheel zoom must keep X following at its original time span");
    require(near(later.y.Min, zoomed.y.Min) && near(later.y.Max, zoomed.y.Max),
            "Y-axis wheel zoom must disable autofit across subsequent taller data");
    require(!f.popup_open(), "Y-axis wheel zoom must not open a context menu");
    std::cout << (inside_plot ? "PASS Ctrl-wheel inside plot changes only Y and keeps X live\n" :
                               "PASS wheel over Y-axis labels changes only Y and keeps X live\n");
}

void frozen_wheel_uses_mouse_anchor_and_resume_keeps_scale() {
    Fixture f;
    f.move(f.press_point());
    const double frozen_end = f.latest_time();
    f.press_space();
    f.append_second(20'000, 20.0f);
    f.frame();
    f.frame();
    const auto paused = f.state();
    require(near(paused.x.Max, frozen_end) && paused.x.Max < f.latest_time() - 0.9,
            "Space must pause on a snapshot while the store continues receiving data");
    const auto cursor = f.press_point();
    f.move(cursor);
    const auto before_wheel = f.state();
    const double fraction = (cursor.x - before_wheel.rect.Min.x) / before_wheel.rect.GetWidth();
    const double anchor = before_wheel.x.Min + fraction * before_wheel.x.Size();
    f.wheel(1.0f);
    f.frame();
    const auto zoomed = f.state();
    require(zoomed.x.Size() < paused.x.Size() * 0.99,
            "Wheel must be usable while viewing a paused snapshot");
    require(near((anchor - zoomed.x.Min) / zoomed.x.Size(), fraction),
            "Frozen wheel zoom must use the mouse anchor even when the panel prefers following");

    for (std::uint64_t second = 2; second <= 6; ++second)
        f.append_second(second * 20'000, 40.0f);
    f.frame();
    f.frame();
    const auto later = f.state();
    require(near(later.x.Min, zoomed.x.Min) && near(later.x.Max, zoomed.x.Max),
            "Wheel on a paused snapshot must not resume live following");
    require(later.y.Size() < 5.0,
            "A paused wheel zoom must continue plotting the snapshot rather than taller live data");
    require(f.points_drawn() > 0,
            "Frozen data must remain visible after the live five-second ring overwrites its original interval");
    require(!f.popup_open(), "Paused wheel zoom must not open a context menu");
    f.press_space();
    const auto resumed = f.state();
    require(near(resumed.x.Max, f.latest_time()) && near(resumed.x.Size(), zoomed.x.Size()),
            "Restoring buffer updates must send a following panel to the latest data at its chosen X span");
    require(near(resumed.y.Min, zoomed.y.Min) && near(resumed.y.Max, zoomed.y.Max),
            "Restoring buffer updates must preserve the frozen view's Y limits");
    require(f.points_drawn() > 0, "Restored following panel must render the live ring again");
    f.append_second(140'000, 40.0f);
    f.frame();
    f.frame();
    require(near(f.state().x.Max, f.latest_time()) && near(f.state().x.Size(), zoomed.x.Size()),
            "Following must continue after restoring updates to a frozen buffer");
    std::cout << "PASS frozen wheel uses the mouse anchor, survives ring eviction, and resumes following at the chosen scale\n";
}

void narrow_overview_keeps_true_scale_and_remains_draggable() {
    Fixture f;
    f.press_space();
    f.move(f.press_point());
    f.wheel(8);
    f.wheel(8);
    f.toggle_overview();
    const auto canvas = f.overview_canvas();
    const auto initial = f.state();
    const double pixels = initial.x.Size() / f.latest_time() * canvas.GetWidth();
    require(pixels < 5, "Fixture must have a very narrow viewport");
    require(f.overview_outline_width() <= pixels + 2.01,
            "Selection outline must follow the real span instead of an 18-pixel minimum");
    const auto center = [&](PlotState state) {
        return ImVec2{std::floor(canvas.Min.x + static_cast<float>((state.x.Min + state.x.Max) * 0.5 / f.latest_time()) * canvas.GetWidth()),
                      std::floor(canvas.GetCenter().y)};
    };
    auto press = center(initial);
    f.move(press);
    f.left(true);
    f.move({press.x - 20, press.y});
    f.left(false);
    f.frame();
    const auto moved = f.state();
    require(moved.x.Min < initial.x.Min && near(moved.x.Size(), initial.x.Size()),
            "A narrow frozen selection must still move without resizing");
    f.move(center(moved));
    f.wheel(8);
    f.wheel(8);
    f.frame();
    const auto tiny = f.state();
    require(tiny.x.Size() < moved.x.Size() && f.overview_outline_width() <= 3.01f,
            "Subpixel viewports must collapse to a hairline and continue zooming");
    f.click(f.overview_fraction_point(0.4f));
    require(near(f.state().x.Size(), tiny.x.Size()) && f.state().x.Min < tiny.x.Min,
            "Clicking outside a tiny frozen selection must relocate it without inflating its span");
    require(near(f.state().y.Min, initial.y.Min) && near(f.state().y.Max, initial.y.Max),
            "Overview fine navigation must not change Y");
    std::cout << "PASS narrow overview uses true-width outline, moves, zooms to a hairline and relocates\n";
}
void overview_tracks_selected_channels() {
    Fixture f({{"Sine", "V"}, {"Second", "A"}});
    f.toggle_overview();
    const ImVec4 first{0.94f, 0.20f, 0.18f, 0.95f};
    const ImVec4 second{0.25f, 0.65f, 0.98f, 0.95f};
    require(f.overview_has_color(first) && !f.overview_has_color(second),
            "Overview must initially render only the selected channel");
    f.toggle_channel(1);
    f.require_unitless_legend();
    require(f.overview_has_color(first) && f.overview_has_color(second),
            "Adding a channel must render both signal colors in the open overview");
    f.press_space();
    f.toggle_channel(0);
    require(!f.overview_has_color(first) && f.overview_has_color(second),
            "Frozen overview must also follow channel deselection");
    f.toggle_channel(1);
    require(!f.overview_has_color(first) && !f.overview_has_color(second),
            "No selected channels must leave no stale preview curves");
    f.toggle_channel(0);
    require(f.overview_has_color(first) && !f.overview_has_color(second),
            "Reselecting a channel must restore its frozen preview");
    std::cout << "PASS overview synchronizes all selected channels live and frozen; legends omit units\n";
}
void overview_entry_and_live_span_preserve_following() {
    Fixture f;
    f.toggle_x_link();
    f.add_window_via_menu();
    require(f.overview_window() == nullptr, "Zoom overview must be closed by default");
    // Establish manual Y limits while retaining live following, so the test
    // distinguishes unwanted Y input from normal data-dependent autofitting.
    const auto pan_press = f.press_point();
    f.move(pan_press);
    f.left(true);
    f.move({pan_press.x, pan_press.y + 25.0f});
    f.left(false);
    f.frame();
    const auto initial = f.state();
    const auto other_initial = f.other_panel_state();
    f.toggle_overview();
    require(f.overview_window() != nullptr, "Clicking the Zoom menu item must open its own overview child");
    require(GImPlot->Plots.GetAliveCount() == 2, "Overview preview must not create another ImPlot plot");
    require(near(f.state().x.Min, initial.x.Min) && near(f.state().x.Max, initial.x.Max) &&
            near(f.state().y.Min, initial.y.Min) && near(f.state().y.Max, initial.y.Max),
            "Clicking the Zoom menu item must not pan or reset the main plot");

    f.click(f.overview_fraction_point(0.60f));
    const auto clicked = f.state();
    require(clicked.x.Size() > initial.x.Size() * 1.1 && near(clicked.x.Max, f.latest_time()),
            "Live overview click outside the selection must adjust X span while keeping latest data at the right");
    const auto drag_press = f.overview_selection_point(f.latest_time());
    f.move(drag_press);
    f.left(true);
    f.move({drag_press.x - 25.0f, drag_press.y + 12.0f});
    f.frame();
    const auto dragged = f.state();
    require(dragged.x.Size() > clicked.x.Size() * 1.05 && near(dragged.x.Max, f.latest_time()),
            "Live overview body drag must resize the left boundary, not move the view away from latest data");
    require(near(dragged.y.Min, initial.y.Min) && near(dragged.y.Max, initial.y.Max),
            "Vertical motion during an overview drag must not pan the main plot's Y axis");
    require(near(f.other_panel_state().x.Min, other_initial.x.Min) &&
            near(f.other_panel_state().x.Max, other_initial.x.Max) &&
            near(f.other_panel_state().y.Min, other_initial.y.Min) &&
            near(f.other_panel_state().y.Max, other_initial.y.Max),
            "An overview adjustment must not alter the other panel");

    f.append_second(20'000, 20.0f);
    f.frame();
    f.frame();
    require(near(f.state().x.Max, f.latest_time()) && near(f.state().x.Size(), dragged.x.Size()),
            "A held overview drag must continue following new data without freezing or changing its chosen span");
    f.left(false);
    f.frame();
    // A double click on the overview canvas must remain within that control.
    f.double_click(f.overview_selection_point(f.latest_time()));
    const auto before_data = f.state();
    f.append_second(40'000, 30.0f);
    f.frame();
    f.frame();
    require(near(f.state().x.Max, f.latest_time()) && near(f.state().x.Size(), before_data.x.Size()),
            "Double clicking an overview must not bubble into the main plot's global freeze gesture");
    const auto before_close = f.state();
    f.toggle_overview();
    require(f.overview_window() == nullptr, "Clicking the Zoom menu item again must close the overview");
    require(near(f.state().x.Min, before_close.x.Min) && near(f.state().x.Max, before_close.x.Max) &&
            near(f.state().y.Min, before_close.y.Min) && near(f.state().y.Max, before_close.y.Max),
            "Closing the overview must retain the selected main-plot ranges");
    require(!f.popup_open(), "Overview interactions must not open a plot context menu");
    std::cout << "PASS Zoom opens without panning, changes only the live X span, keeps following, and closes without resetting\n";
}

void frozen_overview_moves_resizes_and_isolates_input() {
    Fixture f;
    f.toggle_auto_y();
    f.toggle_x_link();
    f.add_window_via_menu();
    const double extent_end = f.latest_time();
    f.press_space();
    f.toggle_overview();
    const auto initial = f.state();
    const auto other_initial = f.other_panel_state();
    const auto body = f.overview_selection_point(extent_end);
    f.move(body);
    f.left(true);
    f.move({body.x - 30.0f, body.y + 10.0f});
    f.left(false);
    f.frame();
    const auto moved = f.state();
    require(moved.x.Min < initial.x.Min - 0.01 && near(moved.x.Size(), initial.x.Size()),
            "Frozen overview selection body must translate X while retaining its span");

    const auto left_grip = f.overview_selection_point(extent_end, -1);
    f.move(left_grip);
    f.left(true);
    f.move({left_grip.x + 12.0f, left_grip.y + 8.0f});
    f.left(false);
    f.frame();
    const auto left_resized = f.state();
    require(left_resized.x.Min > moved.x.Min + 0.01 && near(left_resized.x.Max, moved.x.Max),
            "Frozen overview left handle must change only the minimum X limit");
    const auto right_grip = f.overview_selection_point(extent_end, 1);
    f.move(right_grip);
    f.left(true);
    f.move({right_grip.x - 10.0f, right_grip.y - 8.0f});
    f.left(false);
    f.frame();
    const auto resized = f.state();
    require(resized.x.Max < left_resized.x.Max - 0.01 && near(resized.x.Min, left_resized.x.Min),
            "Frozen overview right handle must change only the maximum X limit");
    require(near(resized.y.Min, initial.y.Min) && near(resized.y.Max, initial.y.Max),
            "Frozen overview body and handles must not alter Y");
    require(near(f.other_panel_state().x.Min, other_initial.x.Min) &&
            near(f.other_panel_state().x.Max, other_initial.x.Max) &&
            near(f.other_panel_state().y.Min, other_initial.y.Min) &&
            near(f.other_panel_state().y.Max, other_initial.y.Max),
            "Frozen overview edits must leave the other panel unchanged");

    const auto canvas_point = f.overview_selection_point(extent_end);
    f.move(canvas_point);
    f.right(true);
    f.move({canvas_point.x + 15.0f, canvas_point.y - 10.0f});
    f.right(false);
    f.frame();
    require(near(f.state().x.Min, resized.x.Min) && near(f.state().x.Max, resized.x.Max) &&
            near(f.state().y.Min, resized.y.Min) && near(f.state().y.Max, resized.y.Max),
            "Right drag over the overview must not zoom the underlying main plot");
    require(!f.popup_open(), "Right-button release over the overview must not open the main plot menu");
    f.double_click(f.overview_selection_point(extent_end));
    const auto after_double_click = f.state();
    for (std::uint64_t second = 1; second <= 6; ++second)
        f.append_second(second * 20'000, 30.0f);
    f.frame();
    f.frame();
    require(near(f.state().x.Min, after_double_click.x.Min) && near(f.state().x.Max, after_double_click.x.Max),
            "Overview double click must not restore live updates to a frozen view");
    f.toggle_overview(); // Exclude preview points when checking the snapshot.
    require(f.overview_window() == nullptr && f.points_drawn() > 0,
            "Overview gestures must preserve the frozen main-plot samples even after the live ring wraps");
    require(near(f.state().x.Min, after_double_click.x.Min) && near(f.state().x.Max, after_double_click.x.Max) &&
            near(f.state().y.Min, initial.y.Min) && near(f.state().y.Max, initial.y.Max),
            "Closing a frozen overview must retain the inspected X range and original Y limits");
    std::cout << "PASS frozen overview moves and resizes only X, isolates main-plot gestures, and closes without unfreezing\n";
}

void held_space_toggles_freeze_only_once() {
    Fixture f;
    const auto initial = f.state();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Space, true);
    f.frame();
    for (std::uint64_t second = 1; second <= 6; ++second)
        f.append_second(second * 20'000, 30.0f);
    const auto& io = ImGui::GetIO();
    const int held_frames = static_cast<int>(std::ceil((io.KeyRepeatDelay + 4.0f * io.KeyRepeatRate + 0.2f) * 60.0f));
    for (int i = 0; i < held_frames; ++i) {
        f.frame();
        require(near(f.state().x.Min, initial.x.Min) && near(f.state().x.Max, initial.x.Max) && f.points_drawn() > 0,
                "Holding Space past the key repeat delay must not repeatedly freeze and restore buffer updates");
    }
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Space, false);
    f.frame();
    require(near(f.state().x.Max, initial.x.Max) && f.points_drawn() > 0,
            "Releasing a held Space key must leave the frozen buffer intact");
    f.press_space();
    require(near(f.state().x.Max, f.latest_time()),
            "A separate Space press must restore buffer updates after the held press is released");
    std::cout << "PASS holding Space across the repeat delay changes frozen state only once\n";
}

} // namespace

int main() {
    {
        Fixture f;
        f.toggle_overview();
        const auto before=f.test_store().range();
        f.test_store().append(before.last_frame+4, std::vector<float>(100, .5f));
        for(int i=0;i<12;++i) f.frame();
        require(f.test_store().range().first_frame==before.first_frame, "Missing frames must preserve view history");
        require(std::isfinite(f.state().y.Min) && std::isfinite(f.state().y.Max), "Gap must not poison Y fit");
        const auto* data=ImGui::GetDrawData();
        for(int i=0;i<data->CmdListsCount;++i)
            for(const auto& v:data->CmdLists[i]->VtxBuffer)
                require(std::isfinite(v.pos.x) && std::isfinite(v.pos.y), "Main plot and Zoom must not emit NaN geometry");
        std::cout << "PASS retained gap history with finite main plot, Y fit and overview rendering\n";
    }
    try {
        {
            wave::SignalHub hub;
            auto& audio=hub.register_source("audio","Audio",20000,{{"Audio","","a"}},false);
            auto& cpu=hub.register_source("cpu","CPU",10,{{"CPU","","c"}},false);
            audio.online=cpu.online=true; cpu.offset=7;
            std::vector<float> samples(20000,1), slow(10,2);
            audio.data->append(0,samples); cpu.data->append(0,slow);
            Fixture f({{"Sine","V"}},&hub);
            require(near(f.state().x.Max,0.99995), "Unselected later source must not push live audio away from right edge");
            f.toggle_channel(1); f.frame();
            require(near(f.state().x.Max,7.9), "Selected live sources must retain common timeline alignment");
            cpu.online=false; f.frame();
            require(near(f.state().x.Max,0.99995), "Disconnected source must not drive a selected live stream");
            audio.data->append(20000,samples); f.frame();
            require(near(f.state().x.Max,1.99995), "Remaining live stream must continue following its own newest data");
            std::cout << "PASS live following ignores unselected and disconnected later sources\n";
        }
        {
            Fixture f;
            auto before=f.state();
            f.middle_double_click(f.press_point());
            require(near(f.state().x.Size(),before.x.Size()), "Auto must be disabled while live");
            f.add_window_via_menu(); f.select_other_signal();
            f.press_space(); f.toggle_auto_y();
            before=f.state(); const auto other_y=f.other_panel_state().y;
            const auto point=f.press_point();
            f.middle_double_click(point);
            const auto after=f.state();
            require(near(after.x.Min,1.0/20000) && near(after.x.Max,f.latest_time()), "Auto must show the complete frozen buffer regardless of cursor position");
            require(after.y.Min < -0.9 && after.y.Max > 0.9 && !f.auto_y_enabled(), "Auto must fit Y once without enabling persistent auto-fit");
            require(near(f.other_panel_state().x.Min,after.x.Min) && near(f.other_panel_state().y.Min,other_y.Min), "Auto must link only X");
            f.append_second(40000,50); f.frame();
            require(near(f.state().x.Max,after.x.Max) && near(f.state().y.Max,after.y.Max), "Auto must keep the view frozen");
            std::cout << "PASS frozen middle-double Auto: full buffer, one-shot Y, X link and live exclusion\n";
        }
        {
            Fixture f;
            f.add_window_via_menu(); f.select_other_signal();
            auto before=f.state();
            auto start=ImVec2{before.rect.Min.x+before.rect.GetWidth()*0.2f,before.rect.Min.y+before.rect.GetHeight()*0.25f};
            auto end=ImVec2{before.rect.Min.x+before.rect.GetWidth()*0.7f,before.rect.Min.y+before.rect.GetHeight()*0.75f};
            f.move(start); f.middle(true); f.move(end); f.middle(false);
            require(near(f.state().x.Size(),before.x.Size()), "Live middle drag must not zoom or pause");
            f.press_space(); before=f.state(); const auto other_y=f.other_panel_state().y;
            start={before.rect.Min.x+before.rect.GetWidth()*0.2f,before.rect.Min.y+before.rect.GetHeight()*0.25f};
            end={before.rect.Min.x+before.rect.GetWidth()*0.7f,before.rect.Min.y+before.rect.GetHeight()*0.75f};
            start={std::floor(start.x),std::floor(start.y)};
            end={std::floor(end.x),std::floor(end.y)};
            // Reverse direction must select the same rectangle; no zoom before release.
            f.move(end); f.middle(true); f.move(start);
            require(near(f.state().x.Min,before.x.Min), "Box selection must preview without changing axes");
            f.middle(false); f.frame();
            auto after=f.state();
            const auto expected_x=[&](float pixel){return before.x.Min+(pixel-before.rect.Min.x)/before.rect.GetWidth()*before.x.Size();};
            const auto expected_y=[&](float pixel){return before.y.Max-(pixel-before.rect.Min.y)/before.rect.GetHeight()*before.y.Size();};
            require(near(after.x.Min,expected_x(start.x)) && near(after.x.Max,expected_x(end.x)), "Middle box must zoom to selected X bounds");
            require(near(after.y.Min,expected_y(end.y)) && near(after.y.Max,expected_y(start.y)), "Middle box must zoom to selected Y bounds");
            require(!f.auto_y_enabled() && near(f.other_panel_state().x.Min,after.x.Min) && near(f.other_panel_state().y.Min,other_y.Min), "Box zoom must leave Y auto-fit and link only X");
            f.append_second(40000,10); f.frame();
            require(near(f.state().x.Max,after.x.Max), "Box zoom must remain frozen");
            const auto center=f.state().rect.GetCenter();
            f.move(center); f.middle(true); f.move({center.x+1,center.y+1}); f.middle(false);
            require(near(f.state().x.Size(),after.x.Size()), "Tiny middle drag must not zoom");
            for(int i=0;i<32;++i) f.frame(); // A separate drag, not a middle double click.
            f.move(center); f.middle(true); f.move({center.x+30,center.y+30}); f.press_key(ImGuiKey_Escape); f.middle(false);
            require(near(f.state().x.Size(),after.x.Size()) && !f.popup_open(), "Escape cancels box selection without opening menus");
            std::cout << "PASS frozen middle rectangle: reverse direction, release-only XY zoom, linked X, manual Y, tiny drag and Escape\n";
        }
        {
            wave::SignalHub hub;
            auto& a=hub.register_source("slow","Slow",10,{{"Slow value","","value"}},false);
            auto& b=hub.register_source("fast","Fast",1000,{{"Fast value","","value"}},false);
            a.online=b.online=true; a.anchored=b.anchored=true;
            std::vector<float> slow(10,1), fast(1000,2);
            a.data->append(0,slow); b.data->append(0,fast);
            Fixture f({{"Sine","V"}},&hub);
            f.toggle_channel(1); f.frame();
            require(f.points_drawn()>10 && near(f.state().x.Max,0.999), "Mixed sources must share live plot time");
            f.add_window_via_menu(); f.select_other_signal(); f.frame();
            require(near(f.state().x.Max,f.other_panel_state().x.Max), "Different rates must follow a shared X playhead");
            f.press_space(); const auto frozen=f.state().x;
            a.data->append(10,slow); b.data->append(1000,fast); f.frame();
            require(near(f.state().x.Max,frozen.Max), "All source snapshots must freeze together");
            a.data->clear_channel(0); f.frame();
            require(f.points_drawn()>0, "Clearing one source must retain another source's frozen data");
            f.press_space(); f.frame();
            require(near(f.state().x.Max,1.999), "Resume must follow newest shared time");
            f.clear_buffer_via_menu(true); f.frame();
            const float one=3;
            a.data->append(20,std::span<const float>(&one,1));
            b.data->append(2000,std::span<const float>(&one,1)); f.frame();
            require(f.points_drawn()>0, "Local clear must use each source's own sequence cutoff");
            std::cout << "PASS mixed sources in windows: shared X, atomic pause, scoped clear and resume\n";
        }
        {
            Fixture f;
            f.register_receiver_timed();
            require(near(f.state().x.Max,1.2) && std::isfinite(f.state().y.Min) && f.points_drawn()>0,
                    "Irregular points must render with actual times and finite axes");
            f.move(f.press_point()); f.right(true); f.right(false); f.frame();
            const auto online_height=GImGui->OpenPopupStack.back().Window->Size.y;
            f.source_online(false); f.frame(); f.frame();
            require(GImGui->OpenPopupStack.back().Window->Size.y<online_height,
                    "Disconnected signals must disappear from the selection menu");
            require(f.points_drawn()>0 && near(f.state().x.Max,1.2), "Disconnect must preserve visible history");
            f.source_online(true); f.frame(); f.frame();
            require(near(GImGui->OpenPopupStack.back().Window->Size.y,online_height), "Reconnect must remove disconnected status");
            std::cout << "PASS receiver time view and disconnected/reconnected menu status\n";
        }
        batched_display_is_continuous_and_freezes();
        {
            Fixture f;
            f.toggle_channel(0);
            const auto empty = f.state();
            f.append_second(40000, 1.0f); f.frame();
            require(near(f.state().x.Min, empty.x.Min) && near(f.state().x.Max, empty.x.Max),
                    "Deselecting the last channel must stop automatic X scrolling");
            f.toggle_channel(0);
            require(near(f.state().x.Max, f.latest_time()),
                    "Selecting a channel must restore live X following");
        }
        {
            Fixture f;
            f.press_space();
            require(f.points_drawn()>0, "Frozen view must initially contain data");
            f.clear_channel_from_source(0);
            require(f.points_drawn()==0, "Remote channel clear must hide old frozen data too");
            f.append_second(40000, 1.0f); f.frame();
            require(f.points_drawn()==0, "Remote clear must preserve freeze state");
            f.press_space();
            require(f.points_drawn()>0, "Resume must show only new channel data");
        }
        {
            Fixture f; f.toggle_channel(0); f.register_external_channels();
            require(f.points_drawn()==0, "Metadata/data arrival must not select channels");
            f.toggle_channel(1);
            require(f.points_drawn()>0, "Dynamically registered channel must be selectable");
        }
        remove_windows_and_recreate();
        paused_x_link_is_global_and_optional();
        axis_label_drag_and_wheel();
        live_x_link_tracks_zoom_and_new_data();
        axis_right_drag_locks_axis_and_keeps_arrow();
        independent_y_autofit_and_manual_override();
        held_x_zoom_continuously_renders_y_autofit();
        overview_context_can_close_and_reopen();
        clearing_buffer_preserves_windows_and_pause();
        local_clear_keeps_other_windows_and_shared_history();
        new_windows_inherit_linked_x_immediately();
        global_y_autofit_is_a_one_shot_action();
        right_drag_zoom_and_anchor();
        nonlive_right_drag_retains_mouse_anchors();
        live_left_pan_keeps_following();
        nonlive_left_pan_translates_both_axes();
        right_click_opens_menu();
        returning_to_press_point_still_suppresses_menu();
        legend_click_hides_signal_without_panning();
        legend_double_click_does_not_freeze();
        double_click_freeze_resume_preserves_view(true);
        double_click_freeze_resume_preserves_view(false);
        double_click_freezes_and_resumes_all_panels();
        live_wheel_changes_x_span_and_keeps_following();
        y_axis_wheel_keeps_x_following(false);
        y_axis_wheel_keeps_x_following(true);
        frozen_wheel_uses_mouse_anchor_and_resume_keeps_scale();
        narrow_overview_keeps_true_scale_and_remains_draggable();
        overview_tracks_selected_channels();
        overview_entry_and_live_span_preserve_following();
        frozen_overview_moves_resizes_and_isolates_input();
        held_space_toggles_freeze_only_once();
        std::cout << "All view interaction integration tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "View interaction test failed: " << error.what() << '\n';
        return 1;
    }
}
