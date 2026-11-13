#include "wave_native.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
void check(int status) { if (status) throw std::runtime_error(wave_last_error()); }
struct Demo {
    wave_session* session{};
    wave_stream* stream{};
    wave_window* plot{};
    HWND pause{}, automatic{}, reverse{}, status{};
    std::jthread producer;
    std::atomic<bool> producer_failed{};
    ULONGLONG started = GetTickCount64();
    bool smoke{}, failed{};
    ~Demo() {
        producer.request_stop();
        if (producer.joinable()) producer.join();
        wave_window_destroy(plot); wave_stream_destroy(stream); wave_session_destroy(session);
    }
    void initialize(HWND host) {
        const auto control = [&](const wchar_t* type, const wchar_t* label, DWORD style, int x, int width, int id) {
            HWND result = CreateWindowW(type, label, WS_CHILD | WS_VISIBLE | style, x, 10, width, 28,
                host, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
            SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
            return result;
        };
        pause = control(L"BUTTON", L"暂停显示", BS_PUSHBUTTON, 12, 90, 1);
        automatic = control(L"BUTTON", L"自动 Y", BS_AUTOCHECKBOX, 120, 90, 2);
        reverse = control(L"BUTTON", L"反转滚动", BS_AUTOCHECKBOX, 220, 110, 3);
        SendMessageW(automatic, BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(reverse, BM_SETCHECK, BST_CHECKED, 0);
        control(L"BUTTON", L"1 s", BS_PUSHBUTTON, 350, 65, 4);
        control(L"BUTTON", L"100 ms", BS_PUSHBUTTON, 425, 75, 5);
        control(L"BUTTON", L"10 ms", BS_PUSHBUTTON, 510, 75, 6);
        status = control(L"STATIC", L"模拟数据 · 2 通道 · 20 kSa/s", SS_LEFT, 610, 480, 7);
        check(wave_session_create(50'000, &session));
        const wave_channel channels[] = {{"a", "Channel A", "V"}, {"b", "Channel B", "V"}};
        check(wave_stream_open(session, "demo", "C++ demo", 20'000, channels, 2, &stream));
        check(wave_window_create(session, reinterpret_cast<uintptr_t>(host), &plot));
        check(wave_window_set_reverse_scroll(plot, 1));
        producer = std::jthread([this](std::stop_token stop) {
            std::array<float, 400> values{};
            uint64_t frame = 0, next{};
            auto deadline = std::chrono::steady_clock::now();
            while (!stop.stop_requested()) {
                for (std::size_t i = 0; i < 200; ++i) {
                    const double t = static_cast<double>(frame + i) / 20'000;
                    values[2 * i] = static_cast<float>(std::sin(2 * std::numbers::pi * 8 * t));
                    values[2 * i + 1] = static_cast<float>(.55 * std::sin(2 * std::numbers::pi * 13 * t));
                }
                if (wave_stream_append(stream, frame, values.data(), values.size(), &next)) {
                    producer_failed = true; return;
                }
                frame = next;
                deadline += std::chrono::milliseconds(10);
                std::this_thread::sleep_until(deadline);
            }
        });
    }
    void command(int id) {
        wave_window_state state{};
        check(wave_window_get_state(plot, &state));
        switch (id) {
        case 1: check(wave_window_set_paused(plot, !state.paused)); break;
        case 2:
            if (SendMessageW(automatic, BM_GETCHECK, 0, 0) == BST_CHECKED)
                check(wave_window_set_auto_y(plot, 1));
            else check(wave_window_set_y_range(plot, -1.5, 1.5));
            break;
        case 3: check(wave_window_set_reverse_scroll(plot, SendMessageW(reverse, BM_GETCHECK, 0, 0) == BST_CHECKED)); break;
        case 4: check(wave_window_set_time_span(plot, 1)); break;
        case 5: check(wave_window_set_time_span(plot, .1)); break;
        case 6: check(wave_window_set_time_span(plot, .01)); break;
        }
    }
    void frame(HWND host) {
        if (producer_failed) throw std::runtime_error("Background sample submission failed");
        check(wave_window_render(plot));
        wave_window_state state{}; wave_stream_state data{};
        check(wave_window_get_state(plot, &state)); check(wave_stream_get_state(stream, &data));
        SetWindowTextW(pause, state.paused ? L"恢复实时" : L"暂停显示");
        SendMessageW(automatic, BM_SETCHECK, state.auto_y ? BST_CHECKED : BST_UNCHECKED, 0);
        const auto label = L"模拟 2 通道 · 20 kSa/s · 已接收 " + std::to_wstring(data.next_frame) + L" 帧";
        SetWindowTextW(status, label.c_str());
        if (smoke && GetTickCount64() - started >= 4000) {
            uintptr_t child{}; check(wave_window_get_handle(plot, &child));
            failed = state.plotted_points == 0 || data.next_frame < 50'000 ||
                GetParent(reinterpret_cast<HWND>(child)) != host;
            std::ofstream("cpp-host-smoke.json") << "{\"ok\":" << (failed ? "false" : "true")
                << ",\"received_frames\":" << data.next_frame << ",\"retained_frames\":" << data.frame_count
                << ",\"plotted_points\":" << state.plotted_points << "}\n";
            DestroyWindow(host);
        }
    }
};
Demo* app{};
LRESULT CALLBACK host_proc(HWND host, UINT message, WPARAM wp, LPARAM lp) {
    try {
        switch (message) {
        case WM_SIZE:
            if (app && app->plot) check(wave_window_set_bounds(app->plot, 0, 48, LOWORD(lp), std::max(0, int(HIWORD(lp)) - 48)));
            return 0;
        case WM_COMMAND: if (app && app->plot) app->command(LOWORD(wp)); return 0;
        case WM_TIMER: if (app && app->plot) app->frame(host); return 0;
        case WM_DESTROY: KillTimer(host, 1); PostQuitMessage(0); return 0;
        }
    } catch (const std::exception& error) {
        app->failed = true;
        KillTimer(host, 1);
        MessageBoxA(host, error.what(), "Wave C++ host", MB_ICONERROR);
        DestroyWindow(host); return 0;
    }
    return DefWindowProcW(host, message, wp, lp);
}
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR args, int) {
    SetProcessDPIAware();
    Demo demo; app = &demo;
    demo.smoke = std::string(args).find("--smoke-test") != std::string::npos;
    WNDCLASSW cls{}; cls.hInstance = instance; cls.lpfnWndProc = host_proc;
    cls.lpszClassName = L"WaveCppHostDemo"; cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    if (!RegisterClassW(&cls)) return 1;
    HWND host = CreateWindowW(cls.lpszClassName, L"C++ 宿主 · Wave 嵌入演示", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        60, 80, 1160, 720, nullptr, nullptr, instance, nullptr);
    if (!host) return 1;
    try {
        demo.initialize(host);
        RECT rect{}; GetClientRect(host, &rect);
        check(wave_window_set_bounds(demo.plot, 0, 48, rect.right, rect.bottom - 48));
        if (!demo.smoke) ShowWindow(host, SW_SHOW);
        SetTimer(host, 1, 16, nullptr);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    } catch (const std::exception& error) {
        MessageBoxA(nullptr, error.what(), "Wave C++ host", MB_ICONERROR);
        DestroyWindow(host); return 1;
    }
    return demo.failed ? 1 : 0;
}
