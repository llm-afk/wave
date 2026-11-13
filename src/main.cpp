// Window/DirectX setup adapted from Dear ImGui's MIT-licensed Win32/DX11 example.
#ifndef WINVER
#define WINVER 0x0603 // Expose Win32 pointer types in the bundled MinGW headers.
#endif
#include "tcp_source.hpp"
#include "data_core.hpp"
#include "wave_view.hpp"
#include "win32_touchpad.hpp"
#include "d3d11_surface.hpp"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "implot.h"
#include <windows.h>
#include <d3d11.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace {
wave::win32::D3D11Surface surface;
UINT resize_width{}, resize_height{};
constexpr UINT_PTR move_size_timer = 1;
bool moving_or_sizing = false;
wave::win32::TouchpadInput touchpad;
wave::win32::WheelInput wheel;
std::function<void()> render_frame;

} // namespace

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
LRESULT WINAPI window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (touchpad.handle(message, wparam)) return 0;
    if (ImGui::GetCurrentContext() && wheel.handle(message, wparam, lparam)) return 0;
    if (ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) return 1;
    switch (message) {
    case WM_ENTERSIZEMOVE:
        moving_or_sizing = true;
        SetTimer(window, move_size_timer, 16, nullptr);
        return 0;
    case WM_TIMER:
        // DefWindowProc runs a nested message loop while moving/resizing.
        // Keep rendering on the UI thread even while the outer loop is blocked.
        if (wparam == move_size_timer && moving_or_sizing) {
            if (render_frame) render_frame();
            return 0;
        }
        break;
    case WM_EXITSIZEMOVE:
        moving_or_sizing = false;
        KillTimer(window, move_size_timer);
        if (render_frame) render_frame();
        return 0;
    case WM_DPICHANGED: {
        const auto* rect = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(window, nullptr, rect->left, rect->top,
            rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED) { resize_width = LOWORD(lparam); resize_height = HIWORD(lparam); }
        return 0;
    case WM_SYSCOMMAND:
        if ((wparam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        moving_or_sizing = false;
        KillTimer(window, move_size_timer);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR command_line, int) {
    // --smoke-test runs the actual renderer for a bounded interval and records
    // metrics; it is a developer check, not a benchmark guarantee.
    const bool smoke = std::string(command_line).find("--smoke-test") != std::string::npos;
    ImGui_ImplWin32_EnableDpiAwareness();
    const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY));
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.style = CS_CLASSDC;
    cls.lpfnWndProc = window_proc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
    cls.lpszClassName = L"WaveDemoWindow";
    if (!RegisterClassExW(&cls)) return 1;
    HWND window = CreateWindowW(cls.lpszClassName, L"Wave", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, static_cast<int>(1200 * scale), static_cast<int>(790 * scale),
        nullptr, nullptr, instance, nullptr);
    if (!window || !surface.create(window)) {
        MessageBoxW(nullptr, L"无法初始化 DirectX 11。", L"Wave Demo", MB_ICONERROR);
        surface.cleanup();
        if (window) DestroyWindow(window);
        UnregisterClassW(cls.lpszClassName, instance);
        return 1;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    auto& io = ImGui::GetIO();
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.IniFilename = nullptr; // This minimal demo starts with a predictable layout.
    const char* font = "C:\\Windows\\Fonts\\msyh.ttc";
    if (GetFileAttributesA(font) != INVALID_FILE_ATTRIBUTES) io.Fonts->AddFontFromFileTTF(font, 16);
    else io.Fonts->AddFontDefault();
    float current_dpi_scale = ImGui_ImplWin32_GetDpiScaleForHwnd(window);
    wave::apply_style(current_dpi_scale);
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(surface.device, surface.context);
    touchpad.enable(window);
    wheel.enable(window);

    wave::SignalHub data;
    wave::WaveView view(data);
    std::unique_ptr<wave::tcp::Receiver> receiver;
    auto retry_listener_at = std::chrono::steady_clock::time_point{};
    ShowWindow(window, SW_SHOWDEFAULT);
    UpdateWindow(window);
    bool done = false;
    bool occluded = false;
    bool render_failed = false;
    std::size_t frame_count = 0, max_points = 0, presented_frames = 0;
    std::size_t move_size_frames = 0;
    std::size_t empty_draw_frames = 0, minimum_draw_vertices = static_cast<std::size_t>(-1);
    double frame_work_ms = 0;
    const auto started = std::chrono::steady_clock::now();
    bool rendering = false;
    std::wstring previous_title;
    render_frame = [&] {
        if (rendering || done || render_failed) return;
        const auto fingers = touchpad.take_delta();
        struct FrameGuard {
            bool& active;
            explicit FrameGuard(bool& value) : active(value) { active = true; }
            ~FrameGuard() { active = false; }
        } guard(rendering);
        if (!receiver && std::chrono::steady_clock::now() >= retry_listener_at) {
            try { receiver = std::make_unique<wave::tcp::Receiver>(data); }
            catch (const std::exception&) {
                retry_listener_at = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                SetWindowTextW(window, L"Wave | 暂时无法接收数据，正在重试");
            }
        }
        if (receiver) {
            receiver->poll();
            const auto title = std::wstring(L"Wave");
            if (title != previous_title) { SetWindowTextW(window, title.c_str()); previous_title = title; }
        }
        const float dpi_scale = ImGui_ImplWin32_GetDpiScaleForHwnd(window);
        if (dpi_scale != current_dpi_scale) {
            current_dpi_scale = dpi_scale;
            wave::apply_style(current_dpi_scale);
        }
        // Normal use sleeps when hidden. Smoke mode still exercises frame
        // generation and DX11 submission on non-interactive test desktops.
        if (!smoke && (IsIconic(window) || (occluded && surface.swap_chain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED))) {
            return;
        }
        occluded = false;
        if (resize_width && resize_height) {
            if (!surface.resize(resize_width, resize_height)) { render_failed = true; return; }
            resize_width = resize_height = 0;
        }
        const auto work_started = std::chrono::steady_clock::now();
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        const auto* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        if (ImGui::Begin("Wave", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            view.draw(fingers);
        }
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::Render();
        const auto vertices = static_cast<std::size_t>(ImGui::GetDrawData()->TotalVtxCount);
        minimum_draw_vertices = std::min(minimum_draw_vertices, vertices);
        if (vertices == 0) ++empty_draw_frames;
        const float clear[] = {0, 0, 0, 1};
        surface.context->OMSetRenderTargets(1, &surface.target, nullptr);
        surface.context->ClearRenderTargetView(surface.target, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        frame_work_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - work_started).count();
        ++frame_count;
        if (moving_or_sizing) ++move_size_frames;
        max_points = std::max(max_points, view.points_this_frame());
        // One vertical blank interval: DXGI follows the display containing
        // most of the window, including when it moves to a different monitor.
        const auto result = surface.swap_chain->Present(1, 0);
        occluded = result == DXGI_STATUS_OCCLUDED;
        if (FAILED(result)) { render_failed = true; return; }
        if (result == S_OK) ++presented_frames;
    };
    while (!done && !render_failed) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT) done = true;
        }
        if (done) break;
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        if (smoke && seconds >= 12) break;
        render_frame();
        // Throttle hidden windows without blocking the move/size callback.
        if (occluded || IsIconic(window)) Sleep(20);
    }
    KillTimer(window, move_size_timer);
    moving_or_sizing = false;
    render_frame = {};
    if (receiver) receiver->stop();
    if (smoke) {
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::ofstream report("smoke-test.json");
        report << "{\n  \"sample_rate_per_channel\": " << data.sample_rate()
            << ",\n  \"tcp_frames\": " << (receiver ? receiver->frames.load() : 0)
            << ",\n  \"tcp_batches\": " << (receiver ? receiver->batches.load() : 0)
            << ",\n  \"tcp_rejected\": " << (receiver ? receiver->rejected.load() : 0)
            << ",\n  \"channels\": " << data.channels().size()
            << ",\n  \"sources\": " << data.source_count()
            << ",\n  \"tcp_gaps\": " << (receiver ? receiver->gaps.load() : 0)
            << ",\n  \"rendered_frames\": " << frame_count
            << ",\n  \"empty_draw_frames\": " << empty_draw_frames
            << ",\n  \"minimum_draw_vertices\": " << (frame_count ? minimum_draw_vertices : 0)
            << ",\n  \"move_size_frames\": " << move_size_frames
            << ",\n  \"elapsed_seconds\": " << elapsed
            << ",\n  \"average_render_loop_fps\": " << frame_count / std::max(0.001, elapsed)
            << ",\n  \"presented_frames\": " << presented_frames
            << ",\n  \"average_cpu_frame_ms\": " << frame_work_ms / std::max<std::size_t>(1, frame_count)
            << ",\n  \"max_submitted_plot_points\": " << max_points
            << ",\n  \"render_failed\": " << (render_failed ? "true" : "false") << "\n}\n";
    }
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    surface.cleanup();
    DestroyWindow(window);
    UnregisterClassW(cls.lpszClassName, instance);
    return render_failed || (smoke && frame_count == 0) ? 1 : 0;
}
