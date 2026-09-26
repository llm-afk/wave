#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

namespace wave {

struct AxisRange {
    double min{};
    double max{1};
};

struct PlotRange {
    AxisRange x;
    AxisRange y;
};

struct ZoomAnchor {
    double x{};
    double y{};
};

enum class ZoomAxes { Both, XOnly, YOnly };

inline constexpr double default_zoom_pixels_per_e_fold = 180;

struct DragZoomLimits {
    double sample_rate{20'000};
    double max_x_span{3600};
    double min_y_span{1e-6};
    double max_y_span{1e12};
    // Moving this many logical pixels changes the corresponding span by e.
    double pixels_per_e_fold{default_zoom_pixels_per_e_fold};
};

namespace navigation_detail {

inline double positive_span(double value, double fallback) noexcept {
    // These limits leave ample headroom for both endpoint arithmetic and logs.
    constexpr double smallest = 1e-150;
    constexpr double largest = 1e150;
    if (std::isnan(value) || value <= 0) value = fallback;
    return std::clamp(value, smallest, largest);
}

inline AxisRange valid_range(AxisRange value, double fallback_span) noexcept {
    if (!std::isfinite(value.min) || !std::isfinite(value.max) ||
        !std::isfinite(value.max - value.min) || value.max <= value.min)
        return {0, fallback_span};
    return value;
}

inline AxisRange zoom_axis(AxisRange initial, double anchor, double log_scale,
                           double minimum, double maximum) noexcept {
    initial = valid_range(initial, std::clamp(1.0, minimum, maximum));
    const double original_span = initial.max - initial.min;
    if (log_scale == 0 && original_span >= minimum && original_span <= maximum)
        return initial; // A drag returned to its starting position is exact.

    if (!std::isfinite(anchor)) anchor = initial.min + original_span * 0.5;
    anchor = std::clamp(anchor, initial.min, initial.max);
    const double fraction = (anchor - initial.min) / original_span;

    // Clamp in log space before exp: arbitrarily large mouse deltas cannot
    // overflow, and hitting a limit does not accumulate hidden zoom state.
    const double exponent = std::clamp(std::log(original_span) + log_scale,
                                       std::log(minimum), std::log(maximum));
    const double span = std::clamp(std::exp(exponent), minimum, maximum);
    AxisRange result{anchor - fraction * span, anchor + (1 - fraction) * span};

    // An extreme absolute coordinate may not have enough double precision to
    // represent the requested span. Prefer a finite, usable range to NaN or
    // coincident limits. Normal plot coordinates never need this fallback.
    if (!std::isfinite(result.min) || !std::isfinite(result.max) || result.max <= result.min)
        result = {-fraction * span, (1 - fraction) * span};
    return result;
}

} // namespace navigation_detail

// initial and anchor are captured once when the right button goes down.
// Pass total displacement from that same press on every frame; never feed a
// previous result back as initial. Positive dx zooms into X; negative dy zooms
// into Y (screen Y grows downwards). The press anchor keeps the same fractional
// position in the viewport. Out-of-viewport anchors are clamped to its edge.
// Enabled axes obey span limits; an unselected valid axis remains unchanged.
inline PlotRange zoom_from_drag(PlotRange initial, ZoomAnchor anchor,
                                double total_dx, double total_dy,
                                ZoomAxes axes = ZoomAxes::Both,
                                DragZoomLimits limits = {}) noexcept {
    using namespace navigation_detail;
    const double rate = std::isfinite(limits.sample_rate) && limits.sample_rate > 0
                            ? limits.sample_rate : 20'000;
    const double min_x = positive_span(8.0 / rate, 8.0 / 20'000);
    const double max_x = std::max(min_x, positive_span(limits.max_x_span, 3600));
    const double min_y = positive_span(limits.min_y_span, 1e-6);
    const double max_y = std::max(min_y, positive_span(limits.max_y_span, 1e12));
    const double sensitivity = std::isfinite(limits.pixels_per_e_fold) && limits.pixels_per_e_fold > 0
                                   ? limits.pixels_per_e_fold : default_zoom_pixels_per_e_fold;
    if (std::isnan(total_dx)) total_dx = 0;
    if (std::isnan(total_dy)) total_dy = 0;

    PlotRange result{valid_range(initial.x, std::clamp(1.0, min_x, max_x)),
                     valid_range(initial.y, std::clamp(1.0, min_y, max_y))};
    if (axes != ZoomAxes::YOnly)
        result.x = zoom_axis(result.x, anchor.x, -total_dx / sensitivity, min_x, max_x);
    if (axes != ZoomAxes::XOnly)
        result.y = zoom_axis(result.y, anchor.y, total_dy / sensitivity, min_y, max_y);
    return result;
}

} // namespace wave
