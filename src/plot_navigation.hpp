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
    if (log_scale == 0 && original_span <= maximum)
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

// initial and anchor are captured once when the right button goes down.
// Pass total displacement from that same press on every frame; never feed a
// previous result back as initial. Dragging right zooms into X and dragging up
// zooms into Y (screen Y grows downwards). The press anchor keeps the same fractional
// position in the viewport. Out-of-viewport anchors are clamped to its edge.
// Enabled axes obey maximum spans; an unselected valid axis remains unchanged.
inline PlotRange zoom_from_drag(PlotRange initial, ZoomAnchor anchor,
                                double total_dx, double total_dy,
                                ZoomAxes axes = ZoomAxes::Both,
                                DragZoomLimits limits = {}) noexcept {
    using namespace navigation_detail;
    const double max_x = positive_span(limits.max_x_span, 3600);
    const double max_y = positive_span(limits.max_y_span, 1e12);
    const double sensitivity = std::isfinite(limits.pixels_per_e_fold) && limits.pixels_per_e_fold > 0
                                   ? limits.pixels_per_e_fold : default_zoom_pixels_per_e_fold;
    if (std::isnan(total_dx)) total_dx = 0;
    if (std::isnan(total_dy)) total_dy = 0;

    PlotRange result{valid_range(initial.x, std::min(1.0, max_x)),
                     valid_range(initial.y, std::min(1.0, max_y))};
    if (axes != ZoomAxes::YOnly)
        result.x = zoom_axis(result.x, anchor.x, -total_dx / sensitivity, max_x);
    if (axes != ZoomAxes::XOnly)
        result.y = zoom_axis(result.y, anchor.y, total_dy / sensitivity, max_y);
    return result;
}

} // namespace wave
