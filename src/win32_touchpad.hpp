#pragma once
#include "wave_view.hpp"
#include "imgui.h"
#include <windows.h>
#include <array>
#include <optional>
#include <utility>

namespace wave::win32 {

// A precision touchpad or a remote client can produce mouse input without a
// device handle. Only the legacy wheel message carries the final (possibly
// coalesced) scroll amount. Use raw input as a source hint, never as a second
// motion event. This registration belongs to the standalone demo.
class WheelInput {
public:
    void enable(HWND window) {
        const RAWINPUTDEVICE mouse{1, 2, 0, window}; // Foreground mouse input only.
        RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
        refresh_direction();
    }

    void observe_mouse(USHORT flags, HANDLE device, DWORD time) {
        if (flags & RI_MOUSE_HWHEEL) hint_[0] = {device == nullptr, time};
        if (flags & RI_MOUSE_WHEEL) hint_[1] = {device == nullptr, time};
    }

    // A zero delta or an ordinary mouse wheel stays on the ImGui backend path.
    bool post_touchpad_wheel(UINT message, WPARAM wparam, DWORD time, INPUT_MESSAGE_DEVICE_TYPE source,
                             bool scroll_reversed) {
        const bool horizontal = message == WM_MOUSEHWHEEL;
        if (!horizontal && message != WM_MOUSEWHEEL) return false;
        auto& hint = hint_[horizontal ? 0 : 1];
        // A coalesced wheel can arrive before its last raw packet. Allow two
        // Windows message-clock ticks; reject stale, opposite-axis and sent input.
        const bool device_less = source == IMDT_MOUSE && hint.device_less && time - hint.time <= 32;
        hint = {};
        if (source != IMDT_TOUCHPAD && !device_less) return false;
        const float scroll = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
        if (scroll == 0) return false;
        const float finger = scroll_reversed ? scroll : -scroll;
        // ImGui wheel coordinates grow left/up. Queue the two axes independently.
        ImGui::GetIO().AddMouseWheelEvent(horizontal ? -finger : 0, horizontal ? 0 : finger);
        return true;
    }

    bool handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_INPUT: {
            RAWINPUT raw{};
            UINT size = sizeof(raw);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, &raw, &size,
                                sizeof(RAWINPUTHEADER)) == sizeof(raw) && raw.header.dwType == RIM_TYPEMOUSE)
                observe_mouse(raw.data.mouse.usButtonFlags, raw.header.hDevice, GetMessageTime());
            break; // DefWindowProc must finish processing foreground WM_INPUT.
        }
        case WM_MOUSEHWHEEL:
        case WM_MOUSEWHEEL: {
            INPUT_MESSAGE_SOURCE source{};
            GetCurrentInputMessageSource(&source);
            return post_touchpad_wheel(message, wparam, GetMessageTime(), source.deviceType, scroll_reversed_);
        }
        case WM_SETTINGCHANGE: refresh_direction(); break;
        case WM_KILLFOCUS:
        case WM_CANCELMODE: hint_ = {}; break;
        }
        return false;
    }

private:
    void refresh_direction() {
        DWORD direction{}, size = sizeof(direction);
        const auto result = RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\PrecisionTouchPad", L"ScrollDirection",
            RRF_RT_REG_DWORD, nullptr, &direction, &size);
        scroll_reversed_ = result == ERROR_SUCCESS && direction == 1;
    }

    struct Hint { bool device_less{}; DWORD time{}; };
    std::array<Hint, 2> hint_{};
    bool scroll_reversed_{};
};

// The demo owns its HWND. Embedded hosts can deliver their own two-finger
// translation directly to WaveView without opting other windows into pointer input.
class TouchpadInput {
public:
    bool enable(HWND window) {
        using RegisterWindow = BOOL (WINAPI*)(HWND, BOOL);
        const auto user = GetModuleHandleW(L"user32.dll");
        const auto register_window = reinterpret_cast<RegisterWindow>(GetProcAddress(user, "RegisterTouchpadCapableWindow"));
        const auto read = reinterpret_cast<ReadFrame>(GetProcAddress(user, "GetPointerFrameTouchpadInfo"));
        get_type_ = reinterpret_cast<decltype(get_type_)>(GetProcAddress(user, "GetPointerType"));
        if (!register_window || !get_type_ || !read || !register_window(window, TRUE)) return false;
        read_frame_ = read;
        return true;
    }

    bool handle(UINT message, WPARAM wparam) {
        if (message == WM_KILLFOCUS || message == WM_CANCELMODE || message == WM_POINTERCAPTURECHANGED) {
            last_.reset();
            pending_ = {};
            return false;
        }
        if (!read_frame_ || (message != WM_POINTERDOWN && message != WM_POINTERUPDATE && message != WM_POINTERUP))
            return false;
        const UINT32 id = GET_POINTERID_WPARAM(wparam);
        POINTER_INPUT_TYPE type{};
        if (!get_type_(id, &type) || type != PT_TOUCHPAD) return false;
        // Only a pair is relevant; a larger frame resets the gesture.
        std::array<POINTER_TOUCH_INFO, 5> frame;
        UINT32 count = static_cast<UINT32>(frame.size());
        if (!read_frame_(id, &count, frame.data()) || count > frame.size()) {
            last_.reset();
            pending_ = {};
        } else {
            update(frame.data(), count);
        }
        // DefWindowProc otherwise promotes these same contacts to wheel messages.
        return true;
    }

    TouchpadDelta take_delta() { return std::exchange(pending_, {}); }

    void update(const POINTER_TOUCH_INFO* frame, UINT32 count) {
        const POINTER_INFO* a = nullptr;
        const POINTER_INFO* b = nullptr;
        for (UINT32 i = 0; i < count; ++i) {
            const auto& p = frame[i].pointerInfo;
            if (p.pointerFlags & POINTER_FLAG_CANCELED) {
                last_.reset();
                pending_ = {};
                return;
            }
            if (!(p.pointerFlags & POINTER_FLAG_INCONTACT)) continue;
            if (!a) a = &p;
            else if (!b) b = &p;
            else { last_.reset(); return; }
        }
        if (!b) { last_.reset(); return; }
        if (a->pointerId > b->pointerId) std::swap(a, b);
        const Pair next{a->sourceDevice, a->pointerId, b->pointerId,
            (double(a->ptHimetricLocation.x) + b->ptHimetricLocation.x) * .5,
            (double(a->ptHimetricLocation.y) + b->ptHimetricLocation.y) * .5};
        if (last_ && last_->device == next.device && last_->first == next.first && last_->second == next.second) {
            // Device coordinates grow right/down, in 0.01 mm. Match the view's
            // 96-DPI logical pixels independently of desktop scrolling preferences.
            constexpr double dip_per_himetric = 96.0 / 2540.0;
            pending_.right += static_cast<float>((next.x - last_->x) * dip_per_himetric);
            pending_.up += static_cast<float>((last_->y - next.y) * dip_per_himetric);
        }
        last_ = next;
    }

private:
    using ReadFrame = BOOL (WINAPI*)(UINT32, UINT32*, POINTER_TOUCH_INFO*);
    struct Pair { HANDLE device; UINT32 first, second; double x, y; };
    ReadFrame read_frame_{};
    decltype(&GetPointerType) get_type_{};
    std::optional<Pair> last_;
    TouchpadDelta pending_;
};

} // namespace wave::win32
