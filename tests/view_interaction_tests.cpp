#include "wave_view.hpp"
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
    Fixture() : store(20'000.0, 5.0, {{"Sine", "V"}}) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImPlot::CreateContext();
        auto& io = ImGui::GetIO();
        io.DisplaySize = {1200, 700};
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigInputTrickleEventQueue = false;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
        io.Fonts->AddFontDefault();
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        require(pixels && width > 0 && height > 0, "Font atlas must be ready before NewFrame");
        io.Fonts->SetTexID(static_cast<ImTextureID>(1));
        wave::apply_style();
        append_second(0, 1.0f);
        view = std::make_unique<wave::WaveView>(store);
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
    }

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

    void view_entire_cache() {
        move(press_point());
        right(true);
        right(false);
        frame();
        require(popup_open() && GImGui->OpenPopupStack.back().Window != nullptr,
                "History setup must open the real plot context menu");
        const auto* popup = GImGui->OpenPopupStack.back().Window;
        const auto target = ImHashStr("查看整个缓存", 0, popup->IDStack.back());
        // Read only the focused item ID; select the menu through queued keys,
        // without changing the view's follow flag or other production state.
        move({-1000, -1000});
        for (int i = 0; i < 12 && GImGui->NavId != target; ++i)
            press_key(ImGuiKey_DownArrow);
        require(GImGui->NavId == target, "Keyboard navigation must reach the entire-cache menu item");
        press_key(ImGuiKey_Enter);
        frame();
        require(!popup_open(), "Selecting the entire-cache menu item must close the popup");
        require(state().x.Size() > 0.9 && near(state().x.Min, 0.0),
                "Entire-cache menu must expose the one-second history rather than the live 200 ms span");
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

    ImVec2 zoom_button_point() const {
        const auto rect = state().rect;
        const float scale = ImGui::GetStyle().FontScaleDpi;
        return {std::floor(rect.Max.x - 27.0f * scale),
                std::floor(rect.Min.y + 12.0f * scale)};
    }

    void click(ImVec2 point) {
        move(point);
        left(true);
        left(false);
        frame();
    }

    void toggle_overview() { click(zoom_button_point()); }

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

    void add_window_via_menu() {
        const auto* menu = ImGui::FindWindowByName("##MainMenuBar");
        require(menu != nullptr, "Adding a panel must use the visible main menu");
        // '+ 窗口' is the first menu entry. Aim just inside that entry using
        // the actual menu window bounds rather than a screen-resolution guess.
        move({std::floor(menu->InnerRect.Min.x + ImGui::GetStyle().ItemSpacing.x + 1.0f),
              std::floor(menu->Pos.y + menu->Size.y * 0.5f)});
        left(true);
        left(false);
        for (int i = 0; i < 4; ++i) frame();
        require(GImPlot->Plots.GetAliveCount() == 2, "Clicking '+ 窗口' must create a second panel");
    }

    PlotState other_panel_state() const {
        for (int i = 0; i < GImPlot->Plots.GetMapSize(); ++i) {
            if (const auto* plot = GImPlot->Plots.TryGetMapData(i); plot && plot->ID != plot_id)
                return {plot->PlotRect, plot->Axes[ImAxis_X1].Range, plot->Axes[ImAxis_Y1].Range};
        }
        throw std::runtime_error("Two-panel test must retain a second plot");
    }

    void append_second(std::uint64_t first_frame, float amplitude) {
        std::vector<float> samples(20'000);
        constexpr double tau = 6.2831853071795864769;
        for (std::size_t i = 0; i < samples.size(); ++i)
            samples[i] = amplitude * static_cast<float>(std::sin(tau * 23.0 *
                static_cast<double>(first_frame + i) / 20'000.0));
        store.append(first_frame, samples);
    }

private:
    wave::DataStore store;
    std::unique_ptr<wave::WaveView> view;
    ImGuiID plot_id{};
};

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

void nonlive_right_drag_retains_mouse_anchors(bool paused) {
    Fixture f;
    if (paused) {
        f.press_space();
    } else {
        f.view_entire_cache();
    }
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
            "Paused and history right drags must retain both press-time mouse anchor ratios");
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
    std::cout << (paused ? "PASS paused right drag retains both mouse anchors and remains paused\n" :
                          "PASS history right drag retains both mouse anchors and remains in history\n");
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

void nonlive_left_pan_translates_both_axes(bool paused) {
    Fixture f;
    if (paused) f.press_space();
    else f.view_entire_cache();
    const auto press = f.press_point();
    f.move(press);
    f.left(true);
    const auto initial = f.state();
    f.move({press.x + 80.0f, press.y + 50.0f});
    f.frame();
    const auto panned = f.state();
    require(panned.x.Max < initial.x.Max - 0.001 && near(panned.x.Size(), initial.x.Size()),
            "Paused or history left drag must freely translate X without changing its scale");
    require(panned.y.Min > initial.y.Min + 0.001 && near(panned.y.Size(), initial.y.Size()),
            "Paused or history left drag must freely translate Y without changing its scale");
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
    std::cout << (paused ? "PASS paused left pan freely moves both axes and stays paused\n" :
                          "PASS history left pan freely moves both axes and stays in history\n");
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
        require(near(f.state().y.Min, before_wheel.y.Min) && near(f.state().y.Max, before_wheel.y.Max),
                "Default frozen wheel zoom must preserve the manually selected Y range");
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
    require(near(resumed.y.Min, chosen.y.Min) && near(resumed.y.Max, chosen.y.Max),
            "Double-click resume must preserve both Y limits rather than fit the newer taller data");
    require(f.points_drawn() > 0, "Resuming must render the newest live data again");

    f.append_second(140'000, 40.0f);
    f.frame();
    f.frame();
    const auto later = f.state();
    require(near(later.x.Max, f.latest_time()) && near(later.x.Size(), chosen.x.Size()),
            "Resumed view must keep following new data at the preserved X span");
    require(near(later.y.Min, chosen.y.Min) && near(later.y.Max, chosen.y.Max),
            "Resume must keep Y autofit disabled across later data, even without an earlier manual Y zoom");
    require(!f.popup_open(), "Double-click freeze and resume must not open a context menu");
    std::cout << (interact_while_frozen ? "PASS double-click freeze retains old data, allows XY pan/right drag/wheel, and resumes at the chosen scale\n" :
                                        "PASS double-click freeze survives ring eviction and resumes without resetting Y\n");
}

void double_click_freezes_and_resumes_all_panels() {
    Fixture f;
    f.add_window_via_menu();
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
    require(later.y.Size() > initial.y.Size() * 5.0,
            "An X-only wheel zoom must preserve Y autofit for later taller data");
    require(!f.popup_open(), "Wheel zoom must not open a context menu");
    std::cout << "PASS live wheel changes X span and keeps following new data\n";
}

void y_axis_wheel_keeps_x_following(bool inside_plot) {
    Fixture f;
    f.move(f.y_axis_point(inside_plot));
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
    std::cout << (inside_plot ? "PASS wheel inside the Y-axis band changes only Y and keeps X live\n" :
                               "PASS wheel over Y-axis labels changes only Y and keeps X live\n");
}

void history_wheel_does_not_resume_following() {
    Fixture f;
    f.view_entire_cache();
    f.move(f.press_point());
    const auto manual = f.state();
    f.wheel(1.0f);
    f.frame();
    const auto zoomed = f.state();
    require(zoomed.x.Size() < manual.x.Size() * 0.99,
            "Wheel must still zoom X after the entire-cache menu enters history mode");
    require(zoomed.x.Max < f.latest_time() - 0.001,
            "History wheel must retain its mouse anchor rather than lock to the latest sample");
    require(near(zoomed.y.Min, manual.y.Min) && near(zoomed.y.Max, manual.y.Max),
            "Default wheel in history mode must leave manual Y limits unchanged");

    f.append_second(20'000, 20.0f);
    f.frame();
    f.frame();
    const auto later = f.state();
    require(near(later.x.Min, zoomed.x.Min) && near(later.x.Max, zoomed.x.Max),
            "Wheel in history mode must not restore live following when data arrives");
    require(near(later.y.Min, zoomed.y.Min) && near(later.y.Max, zoomed.y.Max),
            "Wheel in history mode must not fit unrelated live data outside the visible interval");
    require(!f.popup_open(), "History wheel zoom must not open a context menu");
    std::cout << "PASS wheel preserves history mode selected through the context menu\n";
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

void frozen_panels_restore_their_own_follow_preferences() {
    Fixture f;
    f.add_window_via_menu();
    f.view_entire_cache(); // Only the first panel is browsing history.
    f.press_space();
    const auto initial = f.state();
    const auto other_frozen = f.other_panel_state();

    // The menu-selected history panel remains freely navigable while frozen.
    const auto press = f.press_point();
    f.move(press);
    f.left(true);
    f.move({press.x + 80.0f, press.y + 30.0f});
    f.left(false);
    f.frame();
    require(f.state().x.Max < initial.x.Max - 0.001 && f.points_drawn() > 0,
            "A frozen history panel must remain freely pannable over its retained samples");
    const auto zoom_press = f.press_point();
    f.move(zoom_press);
    f.right(true);
    f.move({zoom_press.x + 60.0f, zoom_press.y - 35.0f});
    f.right(false);
    f.frame();
    const auto chosen = f.state();

    for (std::uint64_t second = 1; second <= 6; ++second)
        f.append_second(second * 20'000, 30.0f);
    f.frame();
    f.frame();
    require(f.points_drawn() > 0 && near(f.state().x.Min, chosen.x.Min) && near(f.state().x.Max, chosen.x.Max),
            "Detached frozen panel must retain its inspected data after the live ring wraps");
    require(near(f.other_panel_state().x.Min, other_frozen.x.Min) &&
            near(f.other_panel_state().x.Max, other_frozen.x.Max),
            "A panel that prefers following must also stop moving while the buffer is frozen");

    // Double click now restores global buffer updates. The first panel keeps
    // its menu-selected history range; the other resumes its existing follow.
    f.double_click(f.press_point());
    const auto restored = f.state();
    require(near(restored.x.Min, chosen.x.Min) && near(restored.x.Max, chosen.x.Max) &&
            near(restored.y.Min, chosen.y.Min) && near(restored.y.Max, chosen.y.Max),
            "Restoring buffer updates must retain a detached panel's complete inspected range");
    // The second panel has no selected signal, so zero total plotted points
    // here proves that the first panel stopped using its retained snapshot.
    require(f.points_drawn() == 0,
            "Restored detached panel must read the live ring where its old interval has expired");
    const auto other_restored = f.other_panel_state();
    require(near(other_restored.x.Max, f.latest_time()) && near(other_restored.x.Size(), other_frozen.x.Size()),
            "Restoring updates must resume only the other panel's existing following preference");

    // A subsequent double click freezes a fresh shared snapshot. Space must
    // operate on that same global frozen state rather than an independent one.
    f.double_click(f.press_point());
    const auto refrozen_other = f.other_panel_state();
    f.append_second(140'000, 40.0f);
    f.frame();
    f.frame();
    require(near(f.other_panel_state().x.Min, refrozen_other.x.Min) &&
            near(f.other_panel_state().x.Max, refrozen_other.x.Max),
            "A second double click must freeze updates for the other following panel again");
    f.press_space();
    require(near(f.state().x.Min, chosen.x.Min) && near(f.state().x.Max, chosen.x.Max) &&
            near(f.other_panel_state().x.Max, f.latest_time()),
            "Space and double click must share global freezing while preserving independent follow preferences");
    std::cout << "PASS double click and Space freeze/restore globally while retaining history and following preferences\n";
}

void overview_entry_and_live_span_preserve_following() {
    Fixture f;
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
    require(f.overview_window() != nullptr, "Clicking Zoom must open its own overview child");
    require(GImPlot->Plots.GetAliveCount() == 2, "Overview preview must not create another ImPlot plot");
    require(near(f.state().x.Min, initial.x.Min) && near(f.state().x.Max, initial.x.Max) &&
            near(f.state().y.Min, initial.y.Min) && near(f.state().y.Max, initial.y.Max),
            "Clicking the Zoom entry must not pan or reset the main plot");

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
    require(f.overview_window() == nullptr, "Clicking Zoom again must close the overview");
    require(near(f.state().x.Min, before_close.x.Min) && near(f.state().x.Max, before_close.x.Max) &&
            near(f.state().y.Min, before_close.y.Min) && near(f.state().y.Max, before_close.y.Max),
            "Closing the overview must retain the selected main-plot ranges");
    require(!f.popup_open(), "Overview interactions must not open a plot context menu");
    std::cout << "PASS Zoom opens without panning, changes only the live X span, keeps following, and closes without resetting\n";
}

void frozen_overview_moves_resizes_and_isolates_input() {
    Fixture f;
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
    try {
        right_drag_zoom_and_anchor();
        nonlive_right_drag_retains_mouse_anchors(true);
        nonlive_right_drag_retains_mouse_anchors(false);
        live_left_pan_keeps_following();
        nonlive_left_pan_translates_both_axes(true);
        nonlive_left_pan_translates_both_axes(false);
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
        history_wheel_does_not_resume_following();
        frozen_wheel_uses_mouse_anchor_and_resume_keeps_scale();
        frozen_panels_restore_their_own_follow_preferences();
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
