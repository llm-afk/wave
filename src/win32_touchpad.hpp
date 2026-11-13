#pragma once
#include "wave_view.hpp"
#include <windows.h>
#include <array>
#include <optional>
#include <utility>

namespace wave::win32 {

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
