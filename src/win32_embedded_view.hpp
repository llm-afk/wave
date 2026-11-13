#pragma once
#ifndef WINVER
#define WINVER 0x0603
#endif
#include "wave_native_internal.hpp"
#include "wave_view.hpp"
#include "win32_touchpad.hpp"
#include "d3d11_surface.hpp"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "implot.h"
#include <chrono>
#include <stdexcept>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace wave::win32 {
// Every call and window message restores the caller's GUI contexts.
struct ContextScope {
    ImGuiContext* imgui = ImGui::GetCurrentContext();
    ImPlotContext* implot = ImPlot::GetCurrentContext();
    ContextScope(ImGuiContext* gui = nullptr, ImPlotContext* plot = nullptr) {
        if (gui) ImGui::SetCurrentContext(gui);
        if (plot) ImPlot::SetCurrentContext(plot);
    }
    ~ContextScope() { ImPlot::SetCurrentContext(implot); ImGui::SetCurrentContext(imgui); }
};

class EmbeddedView {
    std::shared_ptr<NativeSession> session_;
    DWORD thread_ = GetCurrentThreadId();
    HWND window_{};
    HINSTANCE module_{};
    std::wstring class_name_;
    bool registered_{}, win32_ready_{}, dx11_ready_{};
    ImGuiContext* gui_{};
    ImPlotContext* plot_{};
    D3D11Surface surface_;
    std::unique_ptr<WaveView> view_;
    TouchpadInput touchpad_;
    UINT width_{}, height_{}, rendered_width_{}, rendered_height_{};
    float dpi_{};
    bool reverse_scroll_{};

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<EmbeddedView*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<EmbeddedView*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wp, lp);
        if (message == WM_NCDESTROY) {
            self->window_ = nullptr;
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            return DefWindowProcW(window, message, wp, lp);
        }
        if (message == WM_SIZE) {
            self->width_ = LOWORD(lp); self->height_ = HIWORD(lp);
            return 0;
        }
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{}; BeginPaint(window, &paint); EndPaint(window, &paint);
            return 0;
        }
        if (self->win32_ready_) {
            ContextScope scope(self->gui_, self->plot_);
            if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN)
                SetFocus(window);
            if (self->touchpad_.handle(message, wp)) return 0;
            if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) {
                const float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
                const float sign = self->reverse_scroll_ ? -1.0f : 1.0f;
                ImGui::GetIO().AddMouseWheelEvent(message == WM_MOUSEHWHEEL ? -sign * delta : 0,
                                                 message == WM_MOUSEWHEEL ? sign * delta : 0);
                return 0;
            }
            if (ImGui_ImplWin32_WndProcHandler(window, message, wp, lp)) return 1;
        }
        return DefWindowProcW(window, message, wp, lp);
    }

public:
    explicit EmbeddedView(std::shared_ptr<NativeSession> session) : session_(std::move(session)) {}
    EmbeddedView(const EmbeddedView&) = delete;
    EmbeddedView& operator=(const EmbeddedView&) = delete;
    ~EmbeddedView() {
        if (window_) DestroyWindow(window_);
        ContextScope scope(gui_, plot_);
        view_.reset();
        if (dx11_ready_) ImGui_ImplDX11_Shutdown();
        if (win32_ready_) ImGui_ImplWin32_Shutdown();
        if (plot_) ImPlot::DestroyContext(plot_);
        if (gui_) ImGui::DestroyContext(gui_);
        if (registered_) UnregisterClassW(class_name_.c_str(), module_);
    }
    bool correct_thread() const { return GetCurrentThreadId() == thread_; }
    HWND handle() const { return window_; }
    void initialize(HWND parent) {
        DWORD process{};
        if (!IsWindow(parent) || GetWindowThreadProcessId(parent, &process) != thread_ || process != GetCurrentProcessId())
            throw std::invalid_argument("Parent must belong to the calling UI thread");
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&window_proc), &module_))
            throw std::runtime_error("Cannot locate Wave module");
        class_name_ = L"WaveEmbedded_" + std::to_wstring(reinterpret_cast<uintptr_t>(this));
        WNDCLASSEXW cls{};
        cls.cbSize = sizeof(cls); cls.lpfnWndProc = window_proc; cls.hInstance = module_;
        cls.lpszClassName = class_name_.c_str(); cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
        if (!RegisterClassExW(&cls)) throw std::runtime_error("Cannot register Wave child control");
        registered_ = true;
        if (!CreateWindowExW(0, class_name_.c_str(), L"Wave", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                             0, 0, 640, 360, parent, nullptr, module_, this))
            throw std::runtime_error("Cannot create Wave child control");
        if (!surface_.create(window_)) throw std::runtime_error("Cannot initialize Wave DirectX surface");
        ContextScope scope;
        gui_ = ImGui::CreateContext();
        plot_ = ImPlot::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr; io.LogFilename = nullptr;
        io.ConfigInputTrickleEventQueue = false;
        const char* font = "C:\\Windows\\Fonts\\msyh.ttc";
        if (GetFileAttributesA(font) != INVALID_FILE_ATTRIBUTES) io.Fonts->AddFontFromFileTTF(font, 16);
        else io.Fonts->AddFontDefault();
        if (!ImGui_ImplWin32_Init(window_)) throw std::runtime_error("Cannot initialize Wave window input");
        win32_ready_ = true;
        if (!ImGui_ImplDX11_Init(surface_.device, surface_.context)) throw std::runtime_error("Cannot initialize Wave renderer");
        dx11_ready_ = true;
        touchpad_.enable(window_);
        const std::lock_guard lock(session_->mutex);
        view_ = std::make_unique<WaveView>(session_->data);
    }
    void set_bounds(int x, int y, int width, int height) {
        if (width < 0 || height < 0) throw std::invalid_argument("Window size must be nonnegative");
        if (!window_ || !SetWindowPos(window_, nullptr, x, y, width, height, SWP_NOACTIVATE | SWP_NOZORDER))
            throw std::runtime_error("Wave child window is unavailable");
    }
    template<class Action> void apply(Action action) {
        ContextScope scope(gui_, plot_);
        const std::lock_guard lock(session_->mutex);
        action(*view_);
    }
    void reverse_scroll(bool enabled) { reverse_scroll_ = enabled; }
    void render() {
        if (!window_) throw std::runtime_error("Wave parent or child window was destroyed");
        if (!width_ || !height_) return;
        ContextScope scope(gui_, plot_);
        if (width_ != rendered_width_ || height_ != rendered_height_) {
            if (!surface_.resize(width_, height_)) throw std::runtime_error("Cannot resize Wave render target");
            rendered_width_ = width_; rendered_height_ = height_;
        }
        const float dpi = ImGui_ImplWin32_GetDpiScaleForHwnd(window_);
        if (dpi != dpi_) { dpi_ = dpi; apply_style(dpi); }
        ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
        const auto* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos); ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        if (ImGui::Begin("Wave", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse)) {
            const auto fingers = touchpad_.take_delta();
            const std::lock_guard lock(session_->mutex);
            const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            view_->draw(fingers, now);
        }
        ImGui::End(); ImGui::PopStyleVar(); ImGui::Render();
        const float clear[] = {0, 0, 0, 1};
        surface_.context->OMSetRenderTargets(1, &surface_.target, nullptr);
        surface_.context->ClearRenderTargetView(surface_.target, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        // The host's timer controls cadence. A busy GPU can skip this presentation.
        const auto result = surface_.swap_chain->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
        if (FAILED(result) && result != DXGI_ERROR_WAS_STILL_DRAWING)
            throw std::runtime_error("Wave DirectX presentation failed");
    }
};
}
