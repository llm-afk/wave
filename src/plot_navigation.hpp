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

inline constexpr double default_zoom_pixels_per_e_fold = 180;

struct DragZoomLimits {
    double max_x_span{3600};
    double max_y_span{1e12};
    // Moving this many logical pixels changes the corresponding span by e.
    double pixels_per_e_fold{default_zoom_pixels_per_e_fold};
};

namespace navigation_detail {

inline double positive_span(double value, double fallback) noexcept {
    // These limits leave ample headroom for both endpoint arithmetic and logs.
    constexpr double smallest = std::numeric_limits<double>::min();
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
                           double maximum) noexcept {
    initial = valid_range(initial, std::min(1.0, maximum));
    const double original_span = initial.max - initial.min;
    if (log_scale == 0)
        return initial; // A drag returned to its starting position is exact.

    if (!std::isfinite(anchor)) anchor = initial.min + original_span * 0.5;
    anchor = std::clamp(anchor, initial.min, initial.max);
    const double fraction = (anchor - initial.min) / original_span;

    // Only machine underflow and the maximum span bound the exponent; there
    // is no sample-count or fixed-value minimum zoom span.
    const double exponent = std::clamp(std::log(original_span) + log_scale,
        std::log(std::numeric_limits<double>::min()), std::log(maximum));
    const double span = std::min(std::exp(exponent), maximum);
    AxisRange result{anchor - fraction * span, anchor + (1 - fraction) * span};

    // At the precision of the anchor, use adjacent representable coordinates
    // rather than collapsing the range or jumping the viewport back to zero.
    if (!std::isfinite(result.min) || !std::isfinite(result.max) || result.max <= result.min) {
        const double below = std::nextafter(anchor, -std::numeric_limits<double>::infinity());
        const double above = std::nextafter(anchor, std::numeric_limits<double>::infinity());
        result = {fraction > 0 && std::isfinite(below) ? below : anchor,
                  fraction < 1 && std::isfinite(above) ? above : anchor};
        if (result.max <= result.min)
            result = {std::isfinite(below) ? below : anchor, std::isfinite(above) ? above : anchor};
    }
    return result;
}

} // namespace navigation_detail

// Right and up magnify X and Y independently around the anchor (screen Y grows down).
// Use total displacement from the press range for a drag, or one frame's displacement
// from the current range for a touchpad or wheel.
inline PlotRange zoom_from_drag(PlotRange initial, ZoomAnchor anchor,
                                double total_dx, double total_dy,
                                DragZoomLimits limits = {}) noexcept {
    using namespace navigation_detail;
    const double max_x = positive_span(limits.max_x_span, 3600);
    const double max_y = positive_span(limits.max_y_span, 1e12);
    const double sensitivity = std::isfinite(limits.pixels_per_e_fold) && limits.pixels_per_e_fold > 0
                                   ? limits.pixels_per_e_fold : default_zoom_pixels_per_e_fold;
    if (std::isnan(total_dx)) total_dx = 0;
    if (std::isnan(total_dy)) total_dy = 0;

    return {zoom_axis(initial.x, anchor.x, -total_dx / sensitivity, max_x),
            zoom_axis(initial.y, anchor.y, total_dy / sensitivity, max_y)};
}

} // namespace wave
